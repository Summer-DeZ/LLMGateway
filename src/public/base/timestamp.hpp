#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

namespace gw {

using Timestamp = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

inline Timestamp nowSteady() {
    return std::chrono::steady_clock::now();
}

inline std::int64_t wallMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::int64_t unixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string formatWallMs(std::int64_t ms) {
    std::time_t secs = static_cast<std::time_t>(ms / 1000);
    int millis = static_cast<int>(ms % 1000);
    if (millis < 0) {
        millis += 1000;
        --secs;
    }
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf, millis);
    return out;
}

inline std::string formatWallSeconds(std::int64_t unix_sec) {
    std::time_t secs = static_cast<std::time_t>(unix_sec);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace gw
