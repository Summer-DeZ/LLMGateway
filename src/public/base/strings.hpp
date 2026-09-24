#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace gw {

inline std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::string trim(std::string_view s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto begin = std::find_if(s.begin(), s.end(), notSpace);
    auto end = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

inline bool istartsWith(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    return iequals(s.substr(0, prefix.size()), prefix);
}

// 在第一个 sep 处切分；没有 sep 时返回 nullopt。
inline std::optional<std::pair<std::string_view, std::string_view>> splitOnce(std::string_view s, char sep) {
    auto pos = s.find(sep);
    if (pos == std::string_view::npos) return std::nullopt;
    return std::make_pair(s.substr(0, pos), s.substr(pos + 1));
}

}  // namespace gw
