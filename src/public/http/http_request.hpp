#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gw::http {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    int version_minor = 1;
    Headers headers;  // 名字已转小写，保留顺序与重复项
    std::string body;

    std::optional<std::string_view> header(std::string_view lower_name) const {
        for (const auto& [name, value] : headers) {
            if (name == lower_name) return value;
        }
        return std::nullopt;
    }

    bool hasHeader(std::string_view lower_name) const { return header(lower_name).has_value(); }

    bool keepAlive() const {
        bool http10 = version_minor == 0;
        auto conn = header("connection");
        if (conn) {
            if (http10) return containsToken(*conn, "keep-alive");
            return !containsToken(*conn, "close");
        }
        return !http10;
    }

    static bool containsToken(std::string_view value, std::string_view token) {
        std::size_t pos = 0;
        while (pos <= value.size()) {
            std::size_t comma = value.find(',', pos);
            std::string_view item = value.substr(pos, comma == std::string_view::npos ? std::string_view::npos
                                                                                      : comma - pos);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
            if (item.size() == token.size()) {
                bool eq = true;
                for (std::size_t i = 0; i < token.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(item[i])) !=
                        std::tolower(static_cast<unsigned char>(token[i]))) {
                        eq = false;
                        break;
                    }
                }
                if (eq) return true;
            }
            if (comma == std::string_view::npos) break;
            pos = comma + 1;
        }
        return false;
    }
};

}  // namespace gw::http
