#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

namespace gw {

inline std::mt19937_64& threadRng() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    return rng;
}

inline std::string randomHex(std::size_t bytes) {
    auto& rng = threadRng();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        auto value = static_cast<std::uint8_t>(rng() & 0xFF);
        out.push_back(kHex[value >> 4]);
        out.push_back(kHex[value & 0xF]);
    }
    return out;
}

// [0, 1) 均匀分布（重试退避抖动等）
inline double randomDouble01() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(threadRng());
}

}  // namespace gw
