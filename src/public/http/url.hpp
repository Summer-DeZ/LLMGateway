#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gw::http {

struct UrlParts {
    std::string host;
    std::uint16_t port = 80;
    std::string path_prefix;
};

// 解析 http://host[:port][/path]；不支持 https（本项目不实现 TLS）、userinfo 与 query。
std::optional<UrlParts> parseBaseUrl(std::string_view url);

}  // namespace gw::http
