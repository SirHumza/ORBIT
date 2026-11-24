#include "torrent_engine.hpp"

#include <libtorrent/alert_types.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fpkgi {
namespace {

const size_t kMaxTorrentBytes = 4 * 1024 * 1024;
const int kTorrentConnectionLimit = 100;
const std::chrono::seconds kNoPeerTimeout(120);
const std::chrono::minutes kStallTimeout(10);

bool secure_work_directory(const std::string& path, std::string& error) {
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        error = std::string("cannot create torrent work directory: ") + std::strerror(errno);
        return false;
    }
    const int directory = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) {
        error = "cannot securely open torrent work directory";
        return false;
    }
    struct stat info;
    const bool valid = fstat(directory, &info) == 0 && S_ISDIR(info.st_mode);
    close(directory);
    if (!valid) error = "torrent work path is not a directory";
    return valid;
}

bool safe_relative_path(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos) return false;
    size_t begin = 0;
    while (begin <= path.size()) {
        const size_t slash = path.find('/', begin);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string component = path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == "..") return false;
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    return true;
}

bool has_pkg_extension(const std::string& path) {
    if (path.size() < 4) return false;
    const size_t offset = path.size() - 4;
    return std::tolower(static_cast<unsigned char>(path[offset])) == '.' &&
           std::tolower(static_cast<unsigned char>(path[offset + 1])) == 'p' &&
           std::tolower(static_cast<unsigned char>(path[offset + 2])) == 'k' &&
           std::tolower(static_cast<unsigned char>(path[offset + 3])) == 'g';
}

bool select_package(const libtorrent::torrent_info& info, uint64_t expected_size,
                    int& selected, std::string& relative_path, std::string& error) {
    selected = -1;
    const libtorrent::file_storage& files = info.files();
    for (int i = 0; i < files.num_files(); ++i) {
        const libtorrent::file_index_t file_index(i);
        const std::string path = files.file_path(file_index);
        if (!safe_relative_path(path) || !has_pkg_extension(path)) continue;
        const int64_t file_size = files.file_size(file_index);
        if (file_size < 0 || static_cast<uint64_t>(file_size) != expected_size) continue;
        if (selected >= 0) {
            error = "torrent contains multiple PKG files matching the catalog size";
            return false;
        }
        selected = i;
        relative_path = path;
    }
    if (selected < 0) {
        error = "torrent has no safe PKG file matching the catalog size";
        return false;
    }
    return true;
}

} // namespace

bool download_via_torrent(const std::string& magnet_uri, const std::string& torrent_data,
                          const std::string& work_directory, const char* destination,
                          uint64_t expected_size, ProgressCallback callback, void* user,
                          std::string& error) {
    error.clear();
    if ((magnet_uri.empty() == torrent_data.empty()) || !destination || expected_size == 0 ||
        expected_size > static_cast<uint64_t>(INT64_MAX) || torrent_data.size() > kMaxTorrentBytes) {
        error = "torrent input, destination, or expected package size is invalid";
        return false;
    }
    if (!secure_work_directory(work_directory, error)) return false;
    struct stat destination_info;
    if (stat(destination, &destination_info) == 0) {
        error = "torrent destination already exists";
        return false;
    }

    try {
        namespace lt = libtorrent;
        lt::settings_pack settings;
        settings.set_str(lt::settings_pack::user_agent, "ORBIT/0.1.0");
        settings.set_bool(lt::settings_pack::enable_dht, true);
        settings.set_bool(lt::settings_pack::enable_lsd, true);
        settings.set_int(lt::settings_pack::download_rate_limit, 0);
        settings.set_int(lt::settings_pack::upload_rate_limit, 0);
        settings.set_int(lt::settings_pack::connections_limit, 200);
        settings.set_int(lt::settings_pack::connection_speed, 60);
        settings.set_int(lt::settings_pack::torrent_connect_boost, 60);
        lt::session session(settings);

        lt::error_code ec;
        lt::add_torrent_params parameters;
        if (!magnet_uri.empty()) {
            parameters = lt::parse_magnet_uri(magnet_uri, ec);
            if (ec) {
                error = std::string("invalid magnet URI: ") + ec.message();
                return false;
            }
        } else {
            std::shared_ptr<lt::torrent_info> info(new lt::torrent_info(
                torrent_data.data(), static_cast<int>(torrent_data.size()), ec));
            if (ec) {
                error = std::string("invalid .torrent metadata: ") + ec.message();
                return false;
            }
            parameters.ti = info;
        }
        parameters.save_path = work_directory;
        lt::torrent_handle handle = session.add_torrent(parameters, ec);
        if (ec || !handle.is_valid()) {
            error = ec ? std::string("cannot start torrent: ") + ec.message()
                       : "torrent session returned an invalid handle";
            return false;
        }
        handle.set_max_connections(kTorrentConnectionLimit);

        std::string package_relative_path;
        bool selected = false;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_progress = started;
        int64_t last_done = -1;
        for (;;) {
            lt::torrent_status status = handle.status();
            if (status.errc) {
                error = std::string("torrent failed: ") + status.errc.message();
                return false;
            }

            std::shared_ptr<const lt::torrent_info> info = handle.torrent_file();
            if (info && !selected) {
                int selected_index = -1;
                if (!select_package(*info, expected_size, selected_index,
                                    package_relative_path, error)) return false;
                std::vector<lt::download_priority_t> priorities(
                    static_cast<size_t>(info->files().num_files()), lt::dont_download);
                priorities[static_cast<size_t>(selected_index)] = lt::top_priority;
                handle.prioritize_files(priorities);
                selected = true;
            }

            if (status.total_wanted_done > last_done) {
                last_done = status.total_wanted_done;
                last_progress = std::chrono::steady_clock::now();
            }
            if (callback) {
                const uint64_t done = status.total_wanted_done > 0
                    ? static_cast<uint64_t>(status.total_wanted_done) : 0;
                const uint64_t wanted = status.total_wanted > 0
                    ? static_cast<uint64_t>(status.total_wanted) : expected_size;
                callback(done, wanted, user);
            }
            if (selected && status.is_finished) break;

            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if (status.num_peers == 0 && now - started > kNoPeerTimeout) {
                error = "no torrent peers found; trying HTTPS source";
                return false;
            }
            if (now - last_progress > kStallTimeout) {
                error = "torrent transfer stalled; trying HTTPS source";
                return false;
            }

            session.wait_for_alert(lt::milliseconds(500));
            std::vector<lt::alert*> alerts;
            session.pop_alerts(&alerts);
            for (size_t i = 0; i < alerts.size(); ++i) {
                lt::torrent_error_alert* torrent_error = lt::alert_cast<lt::torrent_error_alert>(alerts[i]);
                if (torrent_error && torrent_error->handle == handle) {
                    error = std::string("torrent failed: ") + torrent_error->error.message();
                    return false;
                }
            }
        }

        const std::string source = work_directory + "/" + package_relative_path;
        struct stat package_info;
        if (stat(source.c_str(), &package_info) != 0 || !S_ISREG(package_info.st_mode) ||
            package_info.st_size < 0 || static_cast<uint64_t>(package_info.st_size) != expected_size) {
            error = "completed torrent PKG is missing or has the wrong size";
            return false;
        }
        if (rename(source.c_str(), destination) != 0) {
            error = std::string("cannot stage torrent PKG: ") + std::strerror(errno);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("torrent engine error: ") + exception.what();
        return false;
    }
}

} // namespace fpkgi
