#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

namespace gw {

inline std::string randomHex(std::size_t bytes) {
    thread_local std::mt19937_64 rng{std::random_device{}()};
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

}  // namespace gw
