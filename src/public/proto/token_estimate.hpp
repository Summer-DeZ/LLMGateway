#pragma once

#include <cstddef>
#include <string_view>

#include <nlohmann/json.hpp>

#include "proto/neutral.hpp"

namespace gw::proto {

// 与 DAgent TokenEstimator 相同的启发式。只用于限流预留，不得进入记录/指标/成本。
inline std::int64_t estimateTextTokens(std::string_view text) {
    std::int64_t ascii = 0;
    std::int64_t non_ascii = 0;
    for (unsigned char c : text) {
        if (c < 0x80) {
            ++ascii;
        } else if ((c & 0xC0) != 0x80) {
            ++non_ascii;
        }
    }
    return ascii / 4 + non_ascii;
}

inline std::int64_t estimateRequestTokens(const ChatRequest& request) {
    std::int64_t total = 0;
    for (const auto& message : request.messages) {
        total += estimateTextTokens(message.content);
        total += estimateTextTokens(message.reasoning);
        for (const auto& call : message.tool_calls) {
            total += estimateTextTokens(call.name);
            total += estimateTextTokens(call.arguments);
        }
        total += 4;
    }
    for (const auto& tool : request.tools) {
        total += estimateTextTokens(tool.name);
        total += estimateTextTokens(tool.description);
        total += estimateTextTokens(tool.parameters.dump());
    }
    return total;
}

}  // namespace gw::proto
