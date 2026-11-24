#pragma once

#include "http_client.hpp"

#include <stdint.h>
#include <string>

namespace fpkgi {

bool download_via_torrent(const std::string& magnet_uri, const std::string& torrent_data,
                          const std::string& work_directory, const std::string& resume_path,
                          const char* destination,
                          uint64_t expected_size, ProgressCallback callback, void* user,
                          std::string& error);

} // namespace fpkgi
