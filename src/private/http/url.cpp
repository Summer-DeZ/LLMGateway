#include "http/url.hpp"

#include <charconv>

namespace gw::http {

namespace {

std::optional<std::uint16_t> parsePort(std::string_view s) {
    if (s.empty()) return std::nullopt;
    unsigned value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size() || value == 0 || value > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::optional<UrlParts> parseBaseUrl(std::string_view url) {
    UrlParts out;
    if (!url.starts_with("http://")) return std::nullopt;
    url.remove_prefix(7);
    if (url.empty()) return std::nullopt;

    auto slash = url.find('/');
    std::string_view host_port = slash == std::string_view::npos ? url : url.substr(0, slash);
    if (slash != std::string_view::npos) {
        out.path_prefix = std::string(url.substr(slash));
        while (out.path_prefix.size() > 1 && out.path_prefix.back() == '/') out.path_prefix.pop_back();
        if (out.path_prefix == "/") out.path_prefix.clear();
    }
    if (host_port.empty()) return std::nullopt;

    if (host_port.front() == '[') {
        auto close = host_port.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        out.host = std::string(host_port.substr(1, close - 1));
        std::string_view rest = host_port.substr(close + 1);
        if (!rest.empty()) {
            if (!rest.starts_with(':')) return std::nullopt;
            auto port = parsePort(rest.substr(1));
            if (!port) return std::nullopt;
            out.port = *port;
        }
    } else {
        auto colon = host_port.rfind(':');
        if (colon != std::string_view::npos) {
            out.host = std::string(host_port.substr(0, colon));
            auto port = parsePort(host_port.substr(colon + 1));
            if (!port) return std::nullopt;
            out.port = *port;
        } else {
            out.host = std::string(host_port);
        }
    }
    if (out.host.empty()) return std::nullopt;
    if (out.host.find('@') != std::string::npos) return std::nullopt;
    return out;
}

}  // namespace gw::http
