// ORBIT modification (2026-11-24): FPKGi catalog import, durable queue, and native UI.
#include "catalog.hpp"
#include "http_client.hpp"
#include "package_verify.hpp"
#include "pixel_font.hpp"
#include "pkg_installer.hpp"
#include "torrent_engine.hpp"

#include <SDL2/SDL.h>
#include <orbis/libkernel.h>
#include <orbis/SystemService.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <fcntl.h>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace {

using fpkgi::CatalogItem;

const int kWidth = 1920;
const int kHeight = 1080;
const char* kCatalogUrl = "https://raw.githubusercontent.com/SirHumza/ORBIT/main/catalog/catalog.json";
const char* kBundledCatalog = "/app0/catalog.json";
const char* kDataDirectory = "/data/FPKGi";
const char* kCachedCatalog = "/data/FPKGi/catalog.json";
const char* kQueuePath = "/data/FPKGi/queue.json";
const char* kLegacyCatalogDirectory = "/user/data/FPKGi/ContentJSONs";
const char* kDownloadDirectory = "/data/FPKGi/downloads";
const char* kTorrentInputDirectory = "/data/FPKGi/torrents";
const size_t kMaxCatalogBytes = 2 * 1024 * 1024;
const size_t kMaxTorrentBytes = 4 * 1024 * 1024;
const uint64_t kFreeSpaceReserve = 64ULL * 1024ULL * 1024ULL;
const int kInstallPollLimit = 7200;
const uint32_t kInstallPollDelayUs = 500000;
const int kInstallStablePolls = 4;
const int kQueueAutomaticRetries = 5;

const char* kBootLogPaths[] = {
    "/mnt/usb0/FPKGi-Native-boot.log",
    "/mnt/usb1/FPKGi-Native-boot.log",
    "/data/FPKGi-Native-boot.log",
};

void boot_trace(const char* stage, const char* detail = 0) {
    char line[768];
    const int length = std::snprintf(line, sizeof(line), "ORBIT boot: %s%s%s\n",
                                     stage, detail ? " - " : "", detail ? detail : "");
    if (length <= 0) return;
    const size_t bytes = std::min(static_cast<size_t>(length), sizeof(line) - 1);
    std::fwrite(line, 1, bytes, stderr);
    const int write_mode = std::strcmp(stage, "ENTRY") == 0 ? O_TRUNC : O_APPEND;
    for (size_t i = 0; i < sizeof(kBootLogPaths) / sizeof(kBootLogPaths[0]); ++i) {
        const int fd = open(kBootLogPaths[i],
                            O_WRONLY | O_CREAT | write_mode | O_NOFOLLOW, 0600);
        if (fd < 0) continue;
        const ssize_t ignored = write(fd, line, bytes);
        (void)ignored;
        fsync(fd);
        close(fd);
    }
}

struct SharedState {
    pthread_mutex_t mutex;
    bool mutex_ready;
    std::vector<CatalogItem> items;
    std::vector<CatalogItem> queue;
    fpkgi::HttpClient http;
    bool job_running;
    std::string status;
    uint64_t current;
    uint64_t total;
    int32_t last_error_code;

    SharedState()
        : mutex_ready(false), job_running(false), status("STARTING"),
          current(0), total(0), last_error_code(0) {
        mutex_ready = pthread_mutex_init(&mutex, 0) == 0;
    }
    ~SharedState() {
        http.shutdown();
        if (mutex_ready) pthread_mutex_destroy(&mutex);
    }
};

enum JobType { JobRefresh, JobInstall, JobQueue };

struct JobArgs {
    SharedState* state;
    JobType type;
    CatalogItem item;
};

bool ensure_private_directory(const char* path, std::string& error) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        error = std::string("cannot create ") + path + ": " + std::strerror(errno);
        return false;
    }

    // OpenOrbis PS4 lstat delegates to fstatat, whose PS4 backend returns ENOSYS.
    // A no-follow directory descriptor preserves the same validation without that path.
    const int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) {
        const int open_error = errno;
        error = std::string("cannot securely open ") + path + ": " + std::strerror(open_error);
        return false;
    }

    OrbisKernelStat info;
    std::memset(&info, 0, sizeof(info));
    const int32_t stat_result = sceKernelFstat(directory, &info);
    if (stat_result != 0) {
        char code[16];
        std::snprintf(code, sizeof(code), "0X%08X", static_cast<uint32_t>(stat_result));
        close(directory);
        error = std::string("cannot inspect directory ") + path + ": " + code;
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        close(directory);
        error = std::string(path) + " is not a directory";
        return false;
    }
    const int32_t chmod_result = sceKernelFchmod(directory, 0700);
    if (chmod_result != 0) {
        char code[16];
        std::snprintf(code, sizeof(code), "0X%08X", static_cast<uint32_t>(chmod_result));
        close(directory);
        error = std::string("cannot secure ") + path + ": " + code;
        return false;
    }
    close(directory);
    return true;
}

bool ensure_directories(std::string& error) {
    return ensure_private_directory(kDataDirectory, error) &&
           ensure_private_directory(kDownloadDirectory, error) &&
           ensure_private_directory(kTorrentInputDirectory, error);
}

void set_status(SharedState* state, const std::string& status, bool running, int32_t error_code = 0) {
    pthread_mutex_lock(&state->mutex);
    state->status = status;
    state->last_error_code = error_code;
    if (!running) {
        state->current = 0;
        state->total = 0;
    }
    pthread_mutex_unlock(&state->mutex);
}

void finish_job(SharedState* state) {
    pthread_mutex_lock(&state->mutex);
    state->job_running = false;
    state->current = 0;
    state->total = 0;
    pthread_mutex_unlock(&state->mutex);
}

void progress_callback(uint64_t current, uint64_t total, void* user) {
    SharedState* state = static_cast<SharedState*>(user);
    pthread_mutex_lock(&state->mutex);
    state->current = current;
    state->total = total;
    pthread_mutex_unlock(&state->mutex);
}

bool atomic_save_file(const char* path, const std::string& value, std::string& error) {
    const std::string temporary = std::string(path) + ".tmp";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        error = "cannot securely create temporary file";
        return false;
    }
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        error = "cannot open temporary file stream";
        return false;
    }
    bool ok = std::fwrite(value.data(), 1, value.size(), file) == value.size();
    if (ok && std::fflush(file) != 0) ok = false;
    if (ok && fsync(fileno(file)) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        remove(temporary.c_str());
        error = "cannot flush temporary file";
        return false;
    }
    if (rename(temporary.c_str(), path) != 0) {
        remove(temporary.c_str());
        error = "cannot atomically replace catalog cache";
        return false;
    }
    return true;
}

bool atomic_save_catalog(const std::string& value, std::string& error) {
    return atomic_save_file(kCachedCatalog, value, error);
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 0x20) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << static_cast<char>(c);
    }
    return out.str();
}

std::string queue_as_fpkgi_json(const std::vector<CatalogItem>& queue) {
    std::ostringstream out;
    out << "{\"DATA\":{";
    for (size_t i = 0; i < queue.size(); ++i) {
        if (i) out << ',';
        const CatalogItem& item = queue[i];
        out << '\"' << json_escape(item.pkg_url) << "\":{"
            << "\"title_id\":\"" << json_escape(item.title_id) << "\","
            << "\"name\":\"" << json_escape(item.name) << "\","
            << "\"version\":\"" << json_escape(item.version) << "\","
            << "\"size\":\"" << item.size_bytes << "\"";
        if (!item.region.empty()) out << ",\"region\":\"" << json_escape(item.region) << '\"';
        if (!item.release.empty()) out << ",\"release\":\"" << json_escape(item.release) << '\"';
        if (!item.min_fw.empty()) out << ",\"min_fw\":\"" << json_escape(item.min_fw) << '\"';
        if (!item.cover_url.empty()) out << ",\"cover_url\":\"" << json_escape(item.cover_url) << '\"';
        if (!item.sha256.empty()) out << ",\"sha256\":\"" << json_escape(item.sha256) << '\"';
        if (!item.magnet_uri.empty()) out << ",\"magnet_uri\":\"" << json_escape(item.magnet_uri) << '\"';
        if (!item.torrent_file.empty()) out << ",\"torrent_file\":\"" << json_escape(item.torrent_file) << '\"';
        out << '}';
    }
    out << "}}";
    return out.str();
}

bool persist_queue_locked(SharedState& state, std::string& error) {
    return atomic_save_file(kQueuePath, queue_as_fpkgi_json(state.queue), error);
}

bool load_queue(SharedState& state) {
    std::string json;
    std::string error;
    std::vector<CatalogItem> parsed;
    if (!fpkgi::load_text_file(kQueuePath, kMaxCatalogBytes, json, error)) return false;
    if (!fpkgi::parse_catalog(json, parsed, error) ||
        !fpkgi::validate_local_catalog(parsed, error)) {
        state.status = "SAVED QUEUE INVALID - FILE KEPT: " + error;
        return false;
    }
    state.queue.swap(parsed);
    return true;
}

bool file_exists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

bool preflight_space(uint64_t expected_size, bool& known, uint64_t& available, std::string& error) {
    known = false;
    available = 0;
    struct statvfs info;
    std::memset(&info, 0, sizeof(info));
    if (statvfs(kDownloadDirectory, &info) != 0 || info.f_frsize == 0) return true;
    known = true;
    if (info.f_frsize != 0 && static_cast<uint64_t>(info.f_bavail) >
                                  std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(info.f_frsize)) {
        available = std::numeric_limits<uint64_t>::max();
    } else {
        available = static_cast<uint64_t>(info.f_bavail) * static_cast<uint64_t>(info.f_frsize);
    }
    if (expected_size > std::numeric_limits<uint64_t>::max() - kFreeSpaceReserve ||
        available < expected_size + kFreeSpaceReserve) {
        std::ostringstream out;
        out << "insufficient /data space: need package plus 67108864-byte reserve; available " << available;
        error = out.str();
        return false;
    }
    return true;
}

std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0) out << bytes;
    else {
        out.setf(std::ios::fixed);
        out.precision(1);
        out << value;
    }
    out << ' ' << units[unit];
    return out.str();
}

void refresh_catalog(SharedState* state) {
    boot_trace("REFRESH_BEGIN");
    set_status(state, "REFRESHING CATALOG", true);
    std::string error;
    if (!state->http.initialize(error)) {
        boot_trace("HTTP_INIT_FAILED", error.c_str());
        set_status(state, "CATALOG REFRESH FAILED - CURRENT LIST KEPT: " + error, false);
        return;
    }
    boot_trace("HTTP_READY");
    std::string json;
    boot_trace("CATALOG_FETCH_BEGIN");
    if (!state->http.get_text(kCatalogUrl, kMaxCatalogBytes, json, error)) {
        boot_trace("CATALOG_FETCH_FAILED", error.c_str());
        set_status(state, "CATALOG REFRESH FAILED - CURRENT LIST KEPT: " + error, false);
        return;
    }
    boot_trace("CATALOG_FETCH_COMPLETE");

    std::vector<CatalogItem> parsed;
    boot_trace("CATALOG_PARSE_BEGIN");
    if (!fpkgi::parse_catalog(json, parsed, error)) {
        boot_trace("CATALOG_PARSE_FAILED", error.c_str());
        set_status(state, "CATALOG INVALID - CURRENT LIST KEPT: " + error, false);
        return;
    }
    boot_trace("CATALOG_PARSE_COMPLETE");

    boot_trace("CATALOG_SAVE_BEGIN");
    if (!atomic_save_catalog(json, error)) {
        boot_trace("CATALOG_SAVE_FAILED", error.c_str());
        set_status(state, "CATALOG SAVE FAILED - CURRENT LIST KEPT: " + error, false);
        return;
    }
    boot_trace("CATALOG_SAVE_COMPLETE");

    const size_t updated_count = parsed.size();
    std::ostringstream updated_status;
    updated_status << "REMOTE CATALOG VALIDATED: " << updated_count
                   << (updated_count == 1 ? " PACKAGE" : " PACKAGES");
    pthread_mutex_lock(&state->mutex);
    state->items.swap(parsed);
    state->status = updated_status.str();
    state->current = state->total = 0;
    state->last_error_code = 0;
    pthread_mutex_unlock(&state->mutex);
    boot_trace("REFRESH_COMPLETE");
}

bool install_package(SharedState* state, const CatalogItem& item) {
    std::string error;
    const std::string filename = fpkgi::canonical_package_filename(item);
    const std::string pkg_path = std::string(kDownloadDirectory) + "/" + filename;
    const std::string partial_path = pkg_path + ".part";
    const std::string torrent_resume_path = pkg_path + ".torrent.resume";

    fpkgi::PackageVerification verification;
    if (file_exists(pkg_path)) {
        set_status(state, "VERIFYING EXISTING PACKAGE", true);
        if (!fpkgi::verify_package_file(pkg_path.c_str(), item.size_bytes, item.sha256, verification, error)) {
            set_status(state, "EXISTING PACKAGE INVALID - FILE KEPT: " + error, false);
            return false;
        }
        set_status(state, "EXISTING PACKAGE VERIFIED", true);
    } else {
        bool space_known = false;
        uint64_t available = 0;
        if (!preflight_space(item.size_bytes, space_known, available, error)) {
            set_status(state, "DOWNLOAD PREFLIGHT FAILED: " + error, false);
            return false;
        }
        std::string downloaded_path = partial_path;
        bool downloaded_ok = false;
        if (!item.magnet_uri.empty() || !item.torrent_file.empty()) {
            if (!state->http.initialize(error)) {
                set_status(state, "NETWORK INIT FAILED: " + error, false);
                return false;
            }
            std::string torrent_data;
            if (!item.torrent_file.empty()) {
                const std::string torrent_path = std::string(kTorrentInputDirectory) + "/" + item.torrent_file;
                if (!fpkgi::load_text_file(torrent_path.c_str(), kMaxTorrentBytes, torrent_data, error)) {
                    set_status(state, "TORRENT FILE UNAVAILABLE; FALLING BACK TO HTTPS", true);
                }
            }
            if (!item.magnet_uri.empty() || !torrent_data.empty() || file_exists(torrent_resume_path)) {
                const std::string torrent_partial_path = pkg_path + ".torrent.part";
                const std::string torrent_work_directory = pkg_path + ".torrent-work";
                set_status(state, "TORRENTING " + item.name, true);
                error.clear();
                if (fpkgi::download_via_torrent(item.magnet_uri, torrent_data,
                                                torrent_work_directory, torrent_resume_path,
                                                torrent_partial_path.c_str(),
                                                item.size_bytes, progress_callback, state, error)) {
                    set_status(state, "VERIFYING TORRENT PACKAGE", true);
                    if (fpkgi::verify_package_file(torrent_partial_path.c_str(), item.size_bytes,
                                                   item.sha256, verification, error)) {
                        downloaded_path = torrent_partial_path;
                        downloaded_ok = true;
                    } else {
                        remove(torrent_partial_path.c_str());
                        set_status(state, "TORRENT PACKAGE INVALID; FALLING BACK TO HTTPS", true);
                    }
                } else {
                    set_status(state, "TORRENT SOURCE FAILED; FALLING BACK TO HTTPS: " + error, true);
                }
            }
        }

        if (!downloaded_ok) {
            if (!state->http.initialize(error)) {
                set_status(state, "NETWORK INIT FAILED: " + error, false);
                return false;
            }
            set_status(state, space_known ? "DOWNLOADING " + item.name
                                          : "FREE SPACE UNKNOWN; DOWNLOADING " + item.name,
                       true);
            uint64_t downloaded = 0;
            for (int attempt = 0; attempt < 3; ++attempt) {
                error.clear();
                if (state->http.download(item.pkg_url, partial_path.c_str(), item.size_bytes,
                                         progress_callback, state, downloaded, error)) {
                    downloaded_ok = true;
                    break;
                }
                if (attempt < 2) {
                    std::ostringstream retry;
                    retry << "DOWNLOAD FAILED - RETRY " << (attempt + 1) << "/2: " << error;
                    set_status(state, retry.str(), true);
                    sceKernelUsleep(static_cast<uint32_t>((attempt + 1) * 1000000));
                }
            }
        }
        if (!downloaded_ok) {
            set_status(state, "DOWNLOAD FAILED - PARTIAL KEPT: " + error, false);
            return false;
        }

        if (downloaded_path == partial_path) set_status(state, "VERIFYING PACKAGE", true);
        if (!fpkgi::verify_package_file(downloaded_path.c_str(), item.size_bytes, item.sha256,
                                        verification, error)) {
            remove(downloaded_path.c_str());
            set_status(state, "PACKAGE VERIFY FAILED - INVALID FILE REMOVED: " + error, false);
            return false;
        }
        if (file_exists(pkg_path)) {
            set_status(state, "FINAL PACKAGE APPEARED DURING DOWNLOAD - BOTH FILES KEPT", false);
            return false;
        }
        if (rename(downloaded_path.c_str(), pkg_path.c_str()) != 0) {
            set_status(state, "PACKAGE VERIFIED BUT FINAL RENAME FAILED - PARTIAL KEPT", false);
            return false;
        }
        remove(torrent_resume_path.c_str());
    }

    progress_callback(0, 0, state);
    set_status(state, "SUBMITTING VERIFIED PACKAGE TO BGFT", true);
    fpkgi::PackageInstaller installer;
    int32_t error_code = 0;
    int32_t task_id = -1;
    std::string package_title_id;
    if (!installer.enqueue(pkg_path.c_str(), item.name, item.title_id, item.size_bytes,
                           task_id, package_title_id, error, error_code)) {
        set_status(state, "BGFT START FAILED - PKG KEPT: " + error, false, error_code);
        return false;
    }

    {
        std::ostringstream status;
        status << "BGFT TASK " << task_id << " INSTALLING " << package_title_id;
        set_status(state, status.str(), true);
    }

    int stable_polls = 0;
    for (int poll = 0; poll < kInstallPollLimit; ++poll) {
        uint64_t transferred = 0;
        uint64_t total = 0;
        bool bgft_completed = false;
        int32_t task_error = 0;
        if (!installer.progress(task_id, transferred, total, bgft_completed, task_error,
                                error, error_code)) {
            set_status(state, "BGFT PROGRESS FAILED - PKG KEPT: " + error, false, error_code);
            return false;
        }
        progress_callback(transferred, total, state);
        if (task_error != 0) {
            set_status(state, "BGFT TASK FAILED - PKG KEPT", false, task_error);
            return false;
        }

        bool exists = false;
        bool updating = true;
        if (!installer.query_installation(package_title_id, exists, updating, error, error_code)) {
            set_status(state, "INSTALL CHECK FAILED - PKG KEPT: " + error, false, error_code);
            return false;
        }
        if (bgft_completed && exists && !updating) {
            ++stable_polls;
            if (stable_polls >= kInstallStablePolls) {
                if (remove(pkg_path.c_str()) != 0)
                    set_status(state, "INSTALL VERIFIED - LOCAL PKG DELETE FAILED", false);
                else
                    set_status(state, "INSTALL COMPLETE - VERIFIED LOCAL PKG DELETED", false);
                return true;
            }
        } else {
            stable_polls = 0;
        }
        sceKernelUsleep(kInstallPollDelayUs);
    }
    set_status(state, "INSTALL VERIFY TIMEOUT - PKG KEPT", false);
    return false;
}

bool retryable_queue_failure(SharedState* state) {
    pthread_mutex_lock(&state->mutex);
    const bool retryable =
        state->status.compare(0, std::strlen("DOWNLOAD FAILED"), "DOWNLOAD FAILED") == 0 ||
        state->status.compare(0, std::strlen("NETWORK INIT FAILED"), "NETWORK INIT FAILED") == 0;
    pthread_mutex_unlock(&state->mutex);
    return retryable;
}

void process_queue(SharedState* state) {
    for (;;) {
        CatalogItem item;
        pthread_mutex_lock(&state->mutex);
        if (state->queue.empty()) {
            pthread_mutex_unlock(&state->mutex);
            set_status(state, "QUEUE COMPLETE", false);
            return;
        }
        item = state->queue.front();
        pthread_mutex_unlock(&state->mutex);

        int retry = 0;
        while (!install_package(state, item)) {
            if (!retryable_queue_failure(state) || retry >= kQueueAutomaticRetries) return;
            const unsigned int delay_seconds = std::min(300U, 5U << retry);
            std::ostringstream retry_status;
            retry_status << "RETRYING " << item.name << " IN " << delay_seconds
                         << "S (" << (retry + 1) << "/" << kQueueAutomaticRetries << ')';
            set_status(state, retry_status.str(), true);
            sceKernelUsleep(delay_seconds * 1000000U);
            ++retry;
        }

        pthread_mutex_lock(&state->mutex);
        if (!state->queue.empty() && state->queue.front().pkg_url == item.pkg_url)
            state->queue.erase(state->queue.begin());
        std::string save_error;
        const bool saved = persist_queue_locked(*state, save_error);
        pthread_mutex_unlock(&state->mutex);
        if (!saved) {
            set_status(state, "INSTALLED; QUEUE SAVE FAILED: " + save_error, false);
            return;
        }
    }
}

bool start_job(SharedState& state, JobType type, const CatalogItem* item);

bool enqueue_item(SharedState& state, const CatalogItem& item) {
    std::string error;
    pthread_mutex_lock(&state.mutex);
    for (size_t i = 0; i < state.queue.size(); ++i) {
        if (state.queue[i].pkg_url == item.pkg_url) {
            state.status = "ALREADY IN DOWNLOAD QUEUE";
            pthread_mutex_unlock(&state.mutex);
            return false;
        }
    }
    if (state.queue.size() >= 512) {
        state.status = "QUEUE LIMIT REACHED";
        pthread_mutex_unlock(&state.mutex);
        return false;
    }
    state.queue.push_back(item);
    if (!persist_queue_locked(state, error)) {
        state.queue.pop_back();
        state.status = "QUEUE SAVE FAILED: " + error;
        pthread_mutex_unlock(&state.mutex);
        return false;
    }
    state.status = "ADDED TO QUEUE: " + item.name;
    const bool should_start = !state.job_running;
    pthread_mutex_unlock(&state.mutex);
    if (should_start) start_job(state, JobQueue, 0);
    return true;
}

void* job_main(void* raw) {
    JobArgs* args = static_cast<JobArgs*>(raw);
    SharedState* state = args->state;
    const JobType type = args->type;
    const CatalogItem item = args->item;
    delete args;

    if (type == JobRefresh) refresh_catalog(state);
    else if (type == JobQueue) process_queue(state);
    else install_package(state, item);
    finish_job(state);
    return 0;
}

bool start_job(SharedState& state, JobType type, const CatalogItem* item) {
    pthread_mutex_lock(&state.mutex);
    if (state.job_running) {
        pthread_mutex_unlock(&state.mutex);
        return false;
    }
    state.job_running = true;
    state.current = state.total = 0;
    state.last_error_code = 0;
    state.status = type == JobRefresh ? "STARTING CATALOG REFRESH" : "STARTING DOWNLOAD QUEUE";
    pthread_mutex_unlock(&state.mutex);

    JobArgs* args = new JobArgs;
    args->state = &state;
    args->type = type;
    if (item) args->item = *item;
    pthread_t thread;
    if (pthread_create(&thread, 0, job_main, args) != 0) {
        delete args;
        set_status(&state, "COULD NOT CREATE WORKER THREAD", false);
        finish_job(&state);
        return false;
    }
    pthread_detach(thread);
    return true;
}

bool load_legacy_catalogs(SharedState& state, std::string& error) {
    const int directory_fd = open(kLegacyCatalogDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory_fd < 0) return false;
    DIR* directory = fdopendir(directory_fd);
    if (!directory) {
        close(directory_fd);
        error = "cannot enumerate FPKGi ContentJSONs directory";
        return false;
    }

    std::vector<std::string> names;
    struct dirent* entry = 0;
    while ((entry = readdir(directory)) != 0) {
        const std::string name(entry->d_name);
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
            names.push_back(name);
    }
    closedir(directory);
    std::sort(names.begin(), names.end());

    std::vector<CatalogItem> combined;
    size_t total_bytes = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        std::string json;
        std::vector<CatalogItem> parsed;
        const std::string path = std::string(kLegacyCatalogDirectory) + "/" + names[i];
        if (!fpkgi::load_text_file(path.c_str(), kMaxCatalogBytes, json, error) ||
            json.size() > kMaxCatalogBytes - total_bytes ||
            !fpkgi::parse_catalog(json, parsed, error)) {
            if (error.empty()) error = "combined FPKGi catalogs exceed the 2 MiB limit";
            return false;
        }
        total_bytes += json.size();
        if (parsed.size() > 4096 - combined.size()) {
            error = "combined FPKGi catalogs exceed the 4096 package limit";
            return false;
        }
        combined.insert(combined.end(), parsed.begin(), parsed.end());
    }
    if (names.empty()) return false;
    if (!fpkgi::validate_local_catalog(combined, error)) return false;
    state.items.swap(combined);
    state.status = "FPKGI CONTENT JSONS LOADED";
    return true;
}

void load_initial_catalog(SharedState& state) {
    std::string json;
    std::string error;
    std::vector<CatalogItem> parsed;
    if (load_legacy_catalogs(state, error)) return;
    if (fpkgi::load_text_file(kCachedCatalog, kMaxCatalogBytes, json, error) &&
        fpkgi::parse_catalog(json, parsed, error) &&
        fpkgi::validate_local_catalog(parsed, error)) {
        state.items.swap(parsed);
        state.status = "CACHED CATALOG LOADED";
        return;
    }
    json.clear();
    parsed.clear();
    if (fpkgi::load_text_file(kBundledCatalog, kMaxCatalogBytes, json, error) &&
        fpkgi::parse_catalog(json, parsed, error) &&
        fpkgi::validate_local_catalog(parsed, error)) {
        state.items.swap(parsed);
        state.status = "BUNDLED CATALOG LOADED";
    } else {
        state.status = "NO VALID LOCAL CATALOG";
    }
}

void fill(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

std::string truncate_text(const std::string& value, size_t max_chars) {
    if (value.size() <= max_chars) return value;
    if (max_chars < 4) return value.substr(0, max_chars);
    return value.substr(0, max_chars - 3) + "...";
}

void render(SDL_Renderer* renderer, SharedState& state, int selected) {
    const SDL_Color background = {13, 18, 28, 255};
    const SDL_Color sidebar = {20, 28, 41, 255};
    const SDL_Color panel = {25, 35, 50, 255};
    const SDL_Color inset = {17, 24, 35, 255};
    const SDL_Color selected_color = {35, 58, 80, 255};
    const SDL_Color white = {240, 244, 250, 255};
    const SDL_Color muted = {157, 172, 192, 255};
    const SDL_Color faint = {101, 119, 143, 255};
    const SDL_Color accent = {82, 193, 170, 255};
    const SDL_Color divider = {47, 62, 81, 255};

    std::vector<CatalogItem> items;
    std::string status;
    uint64_t current = 0;
    uint64_t total = 0;
    int32_t error_code = 0;
    size_t queue_count = 0;
    bool busy = false;
    pthread_mutex_lock(&state.mutex);
    items = state.items;
    status = state.status;
    current = state.current;
    total = state.total;
    error_code = state.last_error_code;
    queue_count = state.queue.size();
    busy = state.job_running;
    pthread_mutex_unlock(&state.mutex);

    fill(renderer, 0, 0, kWidth, kHeight, background);
    fill(renderer, 0, 0, 256, kHeight, sidebar);
    fill(renderer, 0, 0, 256, 5, accent);
    fill(renderer, 256, 0, 2, 950, divider);

    // Small orbit mark keeps the wordmark recognizable without cover art or glow.
    fill(renderer, 31, 42, 34, 34, accent);
    fill(renderer, 38, 49, 20, 20, sidebar);
    fill(renderer, 46, 45, 20, 6, accent);
    fill(renderer, 59, 52, 6, 20, accent);
    fpkgi::draw_text(renderer, 76, 45, 5, "ORBIT", white);
    fpkgi::draw_text(renderer, 32, 105, 2, "PS4 LIBRARY", faint);
    fill(renderer, 20, 145, 216, 62, selected_color);
    fill(renderer, 20, 145, 4, 62, accent);
    fpkgi::draw_text(renderer, 43, 165, 4, "BROWSE", white);
    fpkgi::draw_text(renderer, 33, 248, 2, "DOWNLOAD QUEUE", muted);
    fpkgi::draw_text(renderer, 204, 248, 2, std::to_string(queue_count), accent);
    fill(renderer, 30, 291, 194, 1, divider);
    fpkgi::draw_text(renderer, 32, 317, 2, "CATALOG SOURCE", faint);
    fpkgi::draw_text(renderer, 32, 347, 3, "FPKGI JSON", white);
    fpkgi::draw_text(renderer, 32, 379, 2, "LOCAL + HTTPS", muted);
    fpkgi::draw_text(renderer, 32, 908, 2, "ORBIT  /  0.1.0", muted);

    fill(renderer, 258, 0, kWidth - 258, 126, panel);
    fpkgi::draw_text(renderer, 300, 30, 5, "BROWSE LIBRARY", white);
    fpkgi::draw_text(renderer, 302, 76, 2, "YOUR FPKGI CATALOGS, READY TO INSTALL", muted);
    fill(renderer, 1640, 35, 220, 48, inset);
    SDL_Rect count_border = {1640, 35, 220, 48};
    SDL_SetRenderDrawColor(renderer, divider.r, divider.g, divider.b, divider.a);
    SDL_RenderDrawRect(renderer, &count_border);
    fpkgi::draw_text(renderer, 1662, 53, 2,
                     std::to_string(items.size()) + " PACKAGES", accent);
    fill(renderer, 258, 125, kWidth - 258, 2, divider);

    fpkgi::draw_text(renderer, 300, 157, 4, "ALL PACKAGES", white);
    fpkgi::draw_text(renderer, 300, 198, 2,
                     std::to_string(items.size()) + (items.size() == 1 ? " ITEM" : " ITEMS"), muted);
    fpkgi::draw_text(renderer, 322, 244, 2, "TITLE", faint);
    fpkgi::draw_text(renderer, 1080, 244, 2, "TITLE ID", faint);
    fpkgi::draw_text(renderer, 1296, 244, 2, "VERSION", faint);
    fpkgi::draw_text(renderer, 1412, 244, 2, "SIZE", faint);
    fill(renderer, 298, 271, 1190, 1, divider);

    if (items.empty()) {
        fill(renderer, 298, 292, 1190, 508, panel);
        SDL_Rect empty_border = {298, 292, 1190, 508};
        SDL_SetRenderDrawColor(renderer, divider.r, divider.g, divider.b, divider.a);
        SDL_RenderDrawRect(renderer, &empty_border);
        fill(renderer, 348, 354, 4, 372, accent);
        fpkgi::draw_text(renderer, 398, 392, 5, "YOUR LIBRARY IS EMPTY", white);
        fpkgi::draw_text(renderer, 400, 447, 3, "ADD AN FPKGI JSON CATALOG TO GET STARTED", muted);
        fpkgi::draw_text(renderer, 400, 516, 2, "SUPPORTED FOLDER", faint);
        fpkgi::draw_text(renderer, 400, 548, 3, "/USER/DATA/FPKGI/CONTENTJSONS", white);
        fpkgi::draw_text(renderer, 400, 612, 2, "THEN PRESS TRIANGLE TO REFRESH THE LIBRARY", muted);
        fpkgi::draw_text(renderer, 400, 666, 2, "ONLY USE PACKAGE SOURCES YOU ARE AUTHORIZED TO ACCESS", faint);
        fpkgi::draw_text(renderer, 400, 710, 2, "OPTIONAL .TORRENT FILES", faint);
        fpkgi::draw_text(renderer, 400, 742, 3, "/DATA/FPKGI/TORRENTS", white);
    } else {
        const int visible = 7;
        int first = std::max(0, selected - visible + 1);
        if (selected < first) first = selected;
        for (int row = 0; row < visible && first + row < static_cast<int>(items.size()); ++row) {
            const int index = first + row;
            const int y = 282 + row * 74;
            const bool focused = index == selected;
            fill(renderer, 298, y, 1190, 68, focused ? selected_color : ((row & 1) ? background : panel));
            if (focused) fill(renderer, 298, y, 4, 68, accent);
            fpkgi::draw_text(renderer, 322, y + 10, 4, truncate_text(items[index].name, 43), white);
            fpkgi::draw_text(renderer, 1080, y + 21, 2, items[index].title_id, focused ? white : muted);
            fpkgi::draw_text(renderer, 1296, y + 21, 2, "V" + items[index].version, focused ? white : muted);
            fpkgi::draw_text(renderer, 1412, y + 21, 2, human_bytes(items[index].size_bytes), focused ? white : muted);
        }
    }

    fill(renderer, 1512, 150, 374, 650, panel);
    fill(renderer, 1512, 150, 4, 650, accent);
    fpkgi::draw_text(renderer, 1544, 180, 4, "DOWNLOAD QUEUE", white);
    fill(renderer, 1544, 233, 160, 34, inset);
    fpkgi::draw_text(renderer, 1556, 244, 2, busy ? "DOWNLOADING" : "STANDING BY",
                     busy ? accent : muted);
    fpkgi::draw_text(renderer, 1544, 304, 2, "ITEMS WAITING", faint);
    fpkgi::draw_text(renderer, 1544, 338, 5, std::to_string(queue_count), white);
    fill(renderer, 1544, 395, 310, 1, divider);
    fpkgi::draw_text(renderer, 1544, 426, 2, "CURRENT TRANSFER", faint);
    fpkgi::draw_text(renderer, 1544, 460, 2, truncate_text(status, 36), white);
    if (error_code) {
        std::ostringstream code;
        code << "ERROR 0X";
        code.setf(std::ios::hex, std::ios::basefield);
        code.width(8);
        code.fill('0');
        code << static_cast<uint32_t>(error_code);
        fpkgi::draw_text(renderer, 1544, 500, 2, code.str(), white);
    }
    if (current || total) {
        const int bar_x = 1544;
        const int bar_y = 530;
        const int bar_w = 310;
        const int bar_h = 10;
        fill(renderer, bar_x, bar_y, bar_w, bar_h, background);
        int filled = 0;
        if (total) filled = static_cast<int>((static_cast<double>(current) / static_cast<double>(total)) * bar_w);
        fill(renderer, bar_x, bar_y, std::max(4, std::min(bar_w, filled)), bar_h, accent);
        std::string progress = human_bytes(current);
        if (total) {
            const int percent = static_cast<int>((static_cast<double>(current) / static_cast<double>(total)) * 100.0);
            std::ostringstream progress_text;
            progress_text << percent << "%   " << progress << " / " << human_bytes(total);
            progress = progress_text.str();
        }
        fpkgi::draw_text(renderer, 1544, 558, 2, progress, muted);
    } else if (queue_count == 0) {
        fpkgi::draw_text(renderer, 1544, 522, 2, "NOTHING IN THE QUEUE", muted);
        fpkgi::draw_text(renderer, 1544, 562, 2, "SELECT A TITLE AND PRESS X", faint);
    } else {
        fpkgi::draw_text(renderer, 1544, 522, 2, "NEXT ITEM STARTS AUTOMATICALLY", muted);
    }

    if (selected >= 0 && selected < static_cast<int>(items.size())) {
        fill(renderer, 298, 820, 1190, 104, panel);
        fill(renderer, 298, 820, 4, 104, accent);
        fpkgi::draw_text(renderer, 322, 837, 2, "SELECTED TITLE", faint);
        fpkgi::draw_text(renderer, 322, 868, 4, truncate_text(items[selected].name, 42), white);
        fpkgi::draw_text(renderer, 1080, 845, 2, items[selected].title_id, muted);
        fpkgi::draw_text(renderer, 1080, 878, 2,
                         "V" + items[selected].version + "  /  " + human_bytes(items[selected].size_bytes), muted);
    }

    fill(renderer, 0, 950, kWidth, 130, panel);
    fill(renderer, 0, 950, kWidth, 2, divider);
    fpkgi::draw_text(renderer, 292, 980, 3, "X  QUEUE", white);
    fpkgi::draw_text(renderer, 535, 980, 3, "TRIANGLE  REFRESH", white);
    fpkgi::draw_text(renderer, 1000, 980, 3, "R1  RETRY QUEUE", white);
    fpkgi::draw_text(renderer, 1430, 980, 3, "CIRCLE  EXIT", white);
    fpkgi::draw_text(renderer, 292, 1028, 2,
                     "QUEUE AND PARTIAL FILES ARE SAVED UNDER /DATA/FPKGI", muted);
}

} // namespace

int main(int, char**) {
    setvbuf(stdout, 0, _IONBF, 0);
    umask(0077);
    boot_trace("ENTRY", "ORBIT VERSION 0.1.0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        boot_trace("SDL_INIT_FAILED", SDL_GetError());
        return 1;
    }
    boot_trace("SDL_INITIALIZED");

    SDL_Window* window = SDL_CreateWindow("ORBIT", SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED, kWidth, kHeight, 0);
    if (!window) {
        boot_trace("SDL_WINDOW_FAILED", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_Surface* surface = SDL_GetWindowSurface(window);
    if (!surface) {
        boot_trace("SDL_SURFACE_FAILED", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 3;
    }
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    if (!renderer) {
        boot_trace("SDL_RENDERER_FAILED", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 4;
    }
    boot_trace("SDL_RENDERER_READY");

    SDL_Joystick* joystick = SDL_NumJoysticks() > 0 ? SDL_JoystickOpen(0) : 0;
    SharedState state;
    if (!state.mutex_ready) {
        boot_trace("MUTEX_INIT_FAILED");
        if (joystick) SDL_JoystickClose(joystick);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 5;
    }
    int selected = 0;
    render(renderer, state, selected);
    if (SDL_UpdateWindowSurface(window) != 0) {
        boot_trace("FIRST_FRAME_FAILED", SDL_GetError());
    } else {
        boot_trace("FIRST_FRAME_PRESENTED");
    }

    std::string storage_error;
    if (ensure_directories(storage_error)) {
        load_initial_catalog(state);
        load_queue(state);
        boot_trace("STORAGE_READY");
    } else {
        pthread_mutex_lock(&state.mutex);
        state.status = "STORAGE DISABLED: " + storage_error;
        pthread_mutex_unlock(&state.mutex);
        boot_trace("STORAGE_FAILED", storage_error.c_str());
    }

    if (!state.queue.empty()) start_job(state, JobQueue, 0);

    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            bool up = false;
            bool down = false;
            bool choose = false;
            bool refresh = false;
            bool retryQueue = false;
            bool quit = false;
            if (event.type == SDL_QUIT) quit = true;
            if (event.type == SDL_KEYDOWN) {
                up = event.key.keysym.sym == SDLK_UP;
                down = event.key.keysym.sym == SDLK_DOWN;
                choose = event.key.keysym.sym == SDLK_RETURN;
                refresh = event.key.keysym.sym == SDLK_r;
                retryQueue = event.key.keysym.sym == SDLK_q;
                quit = event.key.keysym.sym == SDLK_ESCAPE;
            } else if (event.type == SDL_JOYHATMOTION) {
                up = (event.jhat.value & SDL_HAT_UP) != 0;
                down = (event.jhat.value & SDL_HAT_DOWN) != 0;
            } else if (event.type == SDL_JOYBUTTONDOWN) {
                choose = event.jbutton.button == 0;
                quit = event.jbutton.button == 1;
                refresh = event.jbutton.button == 3;
                retryQueue = event.jbutton.button == 5;
            }

            pthread_mutex_lock(&state.mutex);
            const int count = static_cast<int>(state.items.size());
            const bool busy = state.job_running;
            CatalogItem chosen;
            if (count && selected >= 0 && selected < count) chosen = state.items[selected];
            pthread_mutex_unlock(&state.mutex);

            if (up && count) selected = (selected + count - 1) % count;
            if (down && count) selected = (selected + 1) % count;
            if (refresh && !busy) start_job(state, JobRefresh, 0);
            if (choose && count) enqueue_item(state, chosen);
            if (retryQueue && !busy) start_job(state, JobQueue, 0);
            if (quit && !busy) {
                boot_trace("EXIT_REQUESTED");
                const int32_t exit_result = sceSystemServiceLoadExec("exit", 0);
                char exit_detail[32];
                std::snprintf(exit_detail, sizeof(exit_detail), "0X%08X",
                              static_cast<uint32_t>(exit_result));
                boot_trace("EXIT_RETURNED", exit_detail);
                set_status(&state, std::string("SYSTEM EXIT REQUEST RETURNED: ") + exit_detail,
                           false, exit_result);
            }
        }

        pthread_mutex_lock(&state.mutex);
        const int count = static_cast<int>(state.items.size());
        pthread_mutex_unlock(&state.mutex);
        if (count == 0) selected = 0;
        else if (selected >= count) selected = count - 1;

        render(renderer, state, selected);
        SDL_UpdateWindowSurface(window);
        SDL_Delay(16);
    }

}
