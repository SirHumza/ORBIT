// ORBIT modification (2026-11-24): application identity for native HTTP requests.
#include "http_client.hpp"
#include "catalog.hpp"
#include "pem_bundle.hpp"

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <new>
#include <sys/stat.h>

#include <vector>
#include <unistd.h>
extern "C" {
// OpenOrbis v0.5.4 exports these symbols but its public headers omit their parameters.
int32_t fpkgiHttpSetAutoRedirect(int32_t id, int32_t enabled)
    __asm__("sceHttpSetAutoRedirect");
int32_t fpkgiHttpSetRecvTimeOut(int32_t id, uint32_t microseconds)
    __asm__("sceHttpSetRecvTimeOut");
int32_t fpkgiSslTerm(int32_t ssl_context) __asm__("sceSslTerm");
}

namespace fpkgi {
namespace {

const char* kUserAgent = "ORBIT/0.1.0 (OpenOrbis PS4 Homebrew)";
const char* kHbStoreUserAgent =
    "StoreHAX/GL-0x14000000 ORBIT/0.1.0 (OpenOrbis PS4 Homebrew)";
const char kHbStorePrefix[] = "https://api.pkg-zone.com/";
const char* kCaBundlePath = "/app0/assets/cacert.pem";
const size_t kNetPoolSize = 64 * 1024;
const size_t kSslPoolSize = 1024 * 1024;
const size_t kHttpPoolSize = 512 * 1024;
const int kMaxRedirects = 6;
const uint32_t kConnectTimeoutUs = 10 * 1000 * 1000;
const uint32_t kIoTimeoutUs = 30 * 1000 * 1000;
const int32_t kHttpHeaderOverwrite = 0;
const size_t kDownloadBufferSize = 1024 * 1024;

struct RequestHandles {
    int tmpl;
    int conn;
    int req;
    RequestHandles() : tmpl(-1), conn(-1), req(-1) {}
};

struct TrustStore {
    std::vector<std::string> certificates;
    std::vector<SslPem> pem;
    std::vector<SslPem*> roots;
};

void close_request(RequestHandles& handles) {
    if (handles.req >= 0) sceHttpDeleteRequest(handles.req);
    if (handles.conn >= 0) sceHttpDeleteConnection(handles.conn);
    if (handles.tmpl >= 0) sceHttpDeleteTemplate(handles.tmpl);
    handles.req = handles.conn = handles.tmpl = -1;
}

std::string native_error(const char* operation, int32_t code) {
    std::ostringstream out;
    out << operation << " failed (0x";
    out.setf(std::ios::hex, std::ios::basefield);
    out.width(8);
    out.fill('0');
    out << static_cast<uint32_t>(code) << ")";
    return out.str();
}

bool load_https_trust(int http_context, TrustStore*& trust, std::string& error) {
    std::string bundle;
    if (!load_text_file(kCaBundlePath, 1024 * 1024, bundle, error)) {
        error = "cannot read packaged HTTPS CA bundle: " + error;
        return false;
    }

    TrustStore* value = new (std::nothrow) TrustStore;
    if (!value) {
        error = "cannot allocate HTTPS trust store";
        return false;
    }
    if (!parse_pem_bundle(bundle, value->certificates, error)) {
        delete value;
        return false;
    }
    value->pem.reserve(value->certificates.size());
    value->roots.reserve(value->certificates.size());
    for (size_t i = 0; i < value->certificates.size(); ++i) {
        SslPem pem;
        pem.ptr = &value->certificates[i][0];
        pem.size = value->certificates[i].size();
        value->pem.push_back(pem);
    }
    for (size_t i = 0; i < value->pem.size(); ++i) value->roots.push_back(&value->pem[i]);

    const int result = sceHttpsLoadCert(http_context, static_cast<int32_t>(value->roots.size()),
                                        &value->roots[0], 0, 0);
    if (result < 0) {
        error = native_error("sceHttpsLoadCert", result);
        delete value;
        return false;
    }
    trust = value;
    return true;
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n')) --end;
    return value.substr(begin, end - begin);
}

std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c >= 'A' && c <= 'Z') value[i] = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool header_value(const char* headers, size_t header_size, const char* name, std::string& value) {
    if (!headers || !header_size) return false;
    const std::string block(headers, header_size);
    std::istringstream stream(block);
    std::string line;
    const std::string wanted = lower_ascii(name);
    while (std::getline(stream, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (lower_ascii(trim(line.substr(0, colon))) == wanted) {
            value = trim(line.substr(colon + 1));
            return true;
        }
    }
    return false;
}

bool parse_content_range(const std::string& header, uint64_t& first, uint64_t& last,
                         uint64_t& total) {
    const std::string value = lower_ascii(trim(header));
    if (value.compare(0, 6, "bytes ") != 0) return false;
    size_t offset = 6;
    const auto number = [&value, &offset](uint64_t& result) {
        if (offset >= value.size() || value[offset] < '0' || value[offset] > '9') return false;
        result = 0;
        while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') {
            const uint64_t digit = static_cast<uint64_t>(value[offset] - '0');
            if (result > (UINT64_MAX - digit) / 10) return false;
            result = result * 10 + digit;
            ++offset;
        }
        return true;
    };
    if (!number(first) || offset >= value.size() || value[offset++] != '-' ||
        !number(last) || offset >= value.size() || value[offset++] != '/' ||
        !number(total) || offset != value.size()) return false;
    return first <= last && last < total;
}

void wait_if_paused(PauseRequestedCallback pause_requested,
                    WaitWhilePausedCallback wait_while_paused, void* user) {
    while (pause_requested && wait_while_paused && pause_requested(user))
        wait_while_paused(user);
}

bool is_hb_store_origin(const std::string& url) {
    return url.compare(0, sizeof(kHbStorePrefix) - 1, kHbStorePrefix) == 0;
}

const char* user_agent_for_url(const std::string& url) {
    return is_hb_store_origin(url) ? kHbStoreUserAgent : kUserAgent;
}

bool open_get(int http_context, const std::string& url, uint64_t range_start,
              RequestHandles& handles,
              int32_t& status, std::string& redirect, std::string& error) {
    handles.tmpl = sceHttpCreateTemplate(http_context, user_agent_for_url(url),
                                         ORBIS_HTTP_VERSION_1_1, 1);
    if (handles.tmpl < 0) {
        error = native_error("sceHttpCreateTemplate", handles.tmpl);
        return false;
    }
    handles.conn = sceHttpCreateConnectionWithURL(handles.tmpl, url.c_str(), true);
    if (handles.conn < 0) {
        error = native_error("sceHttpCreateConnectionWithURL", handles.conn);
        return false;
    }
    handles.req = sceHttpCreateRequestWithURL(handles.conn, ORBIS_METHOD_GET, url.c_str(), 0);
    if (handles.req < 0) {
        error = native_error("sceHttpCreateRequestWithURL", handles.req);
        return false;
    }

    int policy_result = fpkgiHttpSetAutoRedirect(handles.req, 0);
    if (policy_result < 0) {
        error = native_error("disable automatic redirects", policy_result);
        return false;
    }
    policy_result = sceHttpSetResolveTimeOut(handles.req, kConnectTimeoutUs);
    if (policy_result < 0) {
        error = native_error("set resolve timeout", policy_result);
        return false;
    }
    policy_result = sceHttpSetConnectTimeOut(handles.req, kConnectTimeoutUs);
    if (policy_result < 0) {
        error = native_error("set connect timeout", policy_result);
        return false;
    }
    policy_result = sceHttpSetSendTimeOut(handles.req, kIoTimeoutUs);
    if (policy_result < 0) {
        error = native_error("set send timeout", policy_result);
        return false;
    }
    policy_result = fpkgiHttpSetRecvTimeOut(handles.req, kIoTimeoutUs);
    if (policy_result < 0) {
        error = native_error("set receive timeout", policy_result);
        return false;
    }

    if (range_start > 0) {
        std::ostringstream range;
        range << "bytes=" << range_start << '-';
        policy_result = sceHttpAddRequestHeader(handles.req, "Range", range.str().c_str(),
                                                kHttpHeaderOverwrite);
        if (policy_result < 0) {
            error = native_error("add HTTP Range header", policy_result);
            return false;
        }
        policy_result = sceHttpAddRequestHeader(handles.req, "Accept-Encoding", "identity",
                                                kHttpHeaderOverwrite);
        if (policy_result < 0) {
            error = native_error("request identity encoding", policy_result);
            return false;
        }
    }

    const int send_result = sceHttpSendRequest(handles.req, 0, 0);
    if (send_result < 0) {
        error = native_error("sceHttpSendRequest", send_result);
        return false;
    }
    const int status_result = sceHttpGetStatusCode(handles.req, &status);
    if (status_result < 0) {
        error = native_error("sceHttpGetStatusCode", status_result);
        return false;
    }

    if (status >= 300 && status < 400) {
        char* headers = 0;
        size_t header_size = 0;
        const int header_result = sceHttpGetAllResponseHeaders(handles.req, &headers, &header_size);
        if (header_result < 0) {
            error = native_error("sceHttpGetAllResponseHeaders", header_result);
            return false;
        }
        if (!header_value(headers, header_size, "location", redirect)) {
            error = "redirect response did not contain Location";
            return false;
        }
    }
    return true;
}

bool open_final_get(int http_context, const std::string& initial_url, uint64_t range_start,
                    RequestHandles& handles,
                    int32_t& status, std::string& final_url, std::string& error) {
    if (!validate_http_url(initial_url, error)) return false;
    const bool pin_hb_store_origin = is_hb_store_origin(initial_url);
    std::string url = initial_url;
    for (int redirects = 0; redirects <= kMaxRedirects; ++redirects) {
        std::string location;
        if (!open_get(http_context, url, range_start, handles, status, location, error)) {
            close_request(handles);
            return false;
        }
        if (status < 300 || status >= 400) {
            final_url = url;
            return true;
        }
        close_request(handles);
        if (redirects == kMaxRedirects) {
            error = "too many HTTP redirects";
            return false;
        }
        std::string next;
        if (!resolve_redirect_url(url, location, next, error)) return false;
        if (pin_hb_store_origin && !is_hb_store_origin(next)) {
            error = "HB-Store redirect left the authorized origin";
            return false;
        }
        url.swap(next);
    }
    error = "too many HTTP redirects";
    return false;
}

std::string status_error(int32_t status) {
    std::ostringstream out;
    out << "HTTP request failed with status " << status;
    return out.str();
}

} // namespace

HttpClient::HttpClient()
    : net_pool_(-1), ssl_context_(-1), http_context_(-1), trust_store_(0) {}
HttpClient::~HttpClient() { shutdown(); }

bool HttpClient::initialize(std::string& error) {
    if (http_context_ >= 0) return true;
    int result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    if (result < 0) {
        error = native_error("load NET module", result);
        return false;
    }
    result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    if (result < 0) {
        error = native_error("load HTTP module", result);
        return false;
    }
    result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    if (result < 0) {
        error = native_error("load SSL module", result);
        return false;
    }
    result = sceNetInit();
    if (result < 0) {
        error = native_error("sceNetInit", result);
        return false;
    }
    net_pool_ = sceNetPoolCreate("FPKGiNativeNet", kNetPoolSize, 0);
    if (net_pool_ < 0) {
        error = native_error("sceNetPoolCreate", net_pool_);
        shutdown();
        return false;
    }
    ssl_context_ = sceSslInit(kSslPoolSize);
    if (ssl_context_ < 0) {
        error = native_error("sceSslInit", ssl_context_);
        shutdown();
        return false;
    }
    http_context_ = sceHttpInit(net_pool_, ssl_context_, kHttpPoolSize);
    if (http_context_ < 0) {
        error = native_error("sceHttpInit", http_context_);
        shutdown();
        return false;
    }
    TrustStore* trust = 0;
    if (!load_https_trust(http_context_, trust, error)) {
        shutdown();
        return false;
    }
    trust_store_ = trust;
    return true;
}

void HttpClient::shutdown() {
    if (http_context_ >= 0) sceHttpTerm(http_context_);
    if (trust_store_) delete static_cast<TrustStore*>(trust_store_);
    if (ssl_context_ >= 0) fpkgiSslTerm(ssl_context_);
    if (net_pool_ >= 0) sceNetPoolDestroy(net_pool_);
    trust_store_ = 0;
    http_context_ = ssl_context_ = net_pool_ = -1;
}

bool HttpClient::get_text(const std::string& url, size_t max_bytes, std::string& output,
                          std::string& error) {
    RequestHandles handles;
    int32_t status = 0;
    std::string final_url;
    if (!open_final_get(http_context_, url, 0, handles, status, final_url, error)) return false;
    if (status != 200) {
        error = status_error(status);
        close_request(handles);
        return false;
    }

    output.clear();
    std::vector<char> buffer(32 * 1024);
    for (;;) {
        const int count = sceHttpReadData(handles.req, &buffer[0], static_cast<uint32_t>(buffer.size()));
        if (count < 0) {
            error = native_error("sceHttpReadData", count);
            close_request(handles);
            return false;
        }
        if (count == 0) break;
        if (static_cast<size_t>(count) > max_bytes - output.size()) {
            error = "HTTP response exceeded size limit";
            close_request(handles);
            return false;
        }
        output.append(&buffer[0], static_cast<size_t>(count));
    }
    close_request(handles);
    return true;
}

bool HttpClient::download(const std::string& url, const char* destination, uint64_t expected_size,
                          ProgressCallback callback, void* user,
                          PauseRequestedCallback pause_requested,
                          WaitWhilePausedCallback wait_while_paused,
                          uint64_t& downloaded_bytes, std::string& error) {
    downloaded_bytes = 0;
    if (!destination || expected_size == 0 ||
        expected_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        error = "HTTP package destination or expected size is invalid";
        return false;
    }
    const int fd = open(destination, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) {
        error = "cannot securely create partial package file";
        return false;
    }
    struct stat file_info;
    if (fstat(fd, &file_info) != 0 || !S_ISREG(file_info.st_mode) || file_info.st_size < 0) {
        error = "partial package path is not a regular file";
        close(fd);
        return false;
    }
    uint64_t range_start = static_cast<uint64_t>(file_info.st_size);
    if (range_start > expected_size) {
        if (ftruncate(fd, 0) != 0) {
            error = "cannot reset oversized partial package";
            close(fd);
            return false;
        }
        range_start = 0;
    }
    if (range_start == expected_size) {
        close(fd);
        downloaded_bytes = expected_size;
        if (callback) callback(downloaded_bytes, expected_size, user);
        return true;
    }

    wait_if_paused(pause_requested, wait_while_paused, user);
    RequestHandles handles;
    int32_t status = 0;
    std::string final_url;
    if (!open_final_get(http_context_, url, range_start, handles, status, final_url, error)) {
        close(fd);
        return false;
    }

    uint64_t write_offset = range_start;
    uint64_t response_bytes_expected = expected_size;
    if (status == 206) {
        char* headers = 0;
        size_t header_size = 0;
        std::string content_range;
        uint64_t first = 0;
        uint64_t last = 0;
        uint64_t total = 0;
        const int header_result = sceHttpGetAllResponseHeaders(handles.req, &headers, &header_size);
        if (header_result < 0 ||
            !header_value(headers, header_size, "content-range", content_range) ||
            !parse_content_range(content_range, first, last, total) || first != range_start ||
            total != expected_size) {
            error = "HTTP partial response has an invalid Content-Range";
            close_request(handles);
            close(fd);
            return false;
        }
        response_bytes_expected = last - first + 1;
    } else if (status == 200) {
        write_offset = 0;
        response_bytes_expected = expected_size;
        if (ftruncate(fd, 0) != 0) {
            error = "cannot reset partial package after server ignored Range";
            close_request(handles);
            close(fd);
            return false;
        }
    } else {
        error = status_error(status);
        close_request(handles);
        close(fd);
        return false;
    }

    int content_length_type = 0;
    size_t content_length = 0;
    if (sceHttpGetResponseContentLength(handles.req, &content_length_type, &content_length) >= 0 &&
        static_cast<uint64_t>(content_length) > response_bytes_expected) {
        error = "HTTP response Content-Length exceeded remaining package size";
        close_request(handles);
        close(fd);
        return false;
    }

    if (lseek(fd, static_cast<off_t>(write_offset), SEEK_SET) < 0) {
        error = "cannot seek partial package file";
        close_request(handles);
        close(fd);
        return false;
    }
    downloaded_bytes = write_offset;
    if (callback) callback(downloaded_bytes, expected_size, user);
    uint64_t response_bytes_read = 0;
    std::vector<unsigned char> buffer(kDownloadBufferSize);
    bool ok = true;
    for (;;) {
        wait_if_paused(pause_requested, wait_while_paused, user);
        const int count = sceHttpReadData(handles.req, &buffer[0], static_cast<uint32_t>(buffer.size()));
        if (count < 0) {
            error = native_error("sceHttpReadData", count);
            ok = false;
            break;
        }
        if (count == 0) break;
        const uint64_t amount = static_cast<uint64_t>(count);
        if (response_bytes_read > response_bytes_expected ||
            amount > response_bytes_expected - response_bytes_read ||
            downloaded_bytes > expected_size || amount > expected_size - downloaded_bytes) {
            error = "HTTP response body exceeded expected package size";
            ok = false;
            break;
        }
        size_t written = 0;
        while (written < static_cast<size_t>(count)) {
            const ssize_t amount_written = write(fd, &buffer[written],
                                                 static_cast<size_t>(count) - written);
            if (amount_written < 0 && errno == EINTR) continue;
            if (amount_written <= 0) break;
            written += static_cast<size_t>(amount_written);
        }
        if (written != static_cast<size_t>(count)) {
            error = "failed to write partial package file";
            ok = false;
            break;
        }
        response_bytes_read += amount;
        downloaded_bytes += amount;
        if (callback) callback(downloaded_bytes, expected_size, user);
    }
    if (ok && response_bytes_read != response_bytes_expected) {
        error = "HTTP response ended before the expected package bytes arrived";
        ok = false;
    }
    if (ok && downloaded_bytes != expected_size) {
        error = "HTTP range response ended before the package was complete";
        ok = false;
    }
    if (fsync(fd) != 0 && ok) {
        error = "failed to sync partial package file";
        ok = false;
    }
    if (close(fd) != 0 && ok) {
        error = "failed to close partial package file";
        ok = false;
    }
    close_request(handles);
    return ok;
}

} // namespace fpkgi
