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

// 直接从请求 JSON 估算（OpenAI / Anthropic 通用字段）。与限流预留相同：只允许用于准入预留，
// 不得进入记录、指标和成本（policies.md §4.1）
inline std::int64_t estimateJsonRequestTokens(const nlohmann::json& body) {
    std::int64_t total = 0;
    if (!body.is_object()) return 0;
    auto text = [&total](const nlohmann::json& v) {
        if (v.is_string()) total += estimateTextTokens(v.get_ref<const std::string&>());
    };
    if (auto it = body.find("messages"); it != body.end() && it->is_array()) {
        for (const auto& m : *it) {
            if (!m.is_object()) continue;
            total += 4;
            if (auto c = m.find("content"); c != m.end()) {
                if (c->is_string()) {
                    text(*c);
                } else if (c->is_array()) {
                    for (const auto& part : *c) {
                        if (part.is_object()) {
                            if (auto t = part.find("text"); t != part.end()) text(*t);
                        } else if (part.is_string()) {
                            text(part);
                        }
                    }
                }
            }
            if (auto r = m.find("reasoning_content"); r != m.end()) text(*r);
            if (auto tc = m.find("tool_calls"); tc != m.end() && tc->is_array()) {
                for (const auto& call : *tc) {
                    if (!call.is_object()) continue;
                    if (auto f = call.find("function"); f != call.end() && f->is_object()) {
                        if (auto a = f->find("arguments"); a != f->end()) text(*a);
                    }
                }
            }
        }
    }
    if (auto it = body.find("tools"); it != body.end() && it->is_array()) {
        for (const auto& t : *it) {
            if (!t.is_object()) continue;
            total += 8;
            if (auto f = t.find("function"); f != t.end() && f->is_object()) {
                if (auto d = f->find("description"); d != f->end()) text(*d);
                if (auto p = f->find("parameters"); p != f->end() && p->is_string()) text(*p);
                if (auto p = f->find("parameters"); p != f->end() && !p->is_string()) {
                    total += estimateTextTokens(p->dump());
                }
            }
        }
    }
    return total;
}

}  // namespace gw::proto
