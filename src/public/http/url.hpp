#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gw::http {

struct UrlParts {
    bool tls = false;
    std::string host;
    std::uint16_t port = 80;
    std::string path_prefix;
};

// 解析 http(s)://host[:port][/path]；不支持 userinfo 与 query。
std::optional<UrlParts> parseBaseUrl(std::string_view url);

}  // namespace gw::http
