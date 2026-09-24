#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace gw::proto {

enum class Protocol { openai, anthropic };

enum class Role { system, user, assistant, tool };

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // JSON 字符串
};

struct Message {
    Role role = Role::user;
    std::string content;
    std::string reasoning;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
};

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters;
};

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    nlohmann::json tool_choice;
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::vector<std::string> stop;
    bool stream = false;
    bool client_wants_usage = false;
    std::string user;
    bool has_n_greater_than_one = false;
    std::string unsupported_reason;  // 跨协议不支持时的原因（图片、response_format 等）
};

struct TextDelta {
    std::string text;
};
struct ReasoningDelta {
    std::string text;
};
struct ToolCallBegin {
    int index = 0;
    std::string id;
    std::string name;
};
struct ToolCallArgs {
    int index = 0;
    std::string fragment;
};
struct ToolCallEnd {
    int index = 0;
};
struct Usage {
    std::int64_t prompt = 0;
    std::int64_t completion = 0;
    std::int64_t cached = 0;
};

enum class FinishReason { stop, length, tool_calls, content_filter, error };

struct Finish {
    FinishReason reason = FinishReason::stop;
    std::string raw;
};

using StreamEvent = std::variant<TextDelta, ReasoningDelta, ToolCallBegin, ToolCallArgs, ToolCallEnd, Usage, Finish>;

inline bool isFirstTokenEvent(const StreamEvent& event) {
    if (const auto* text = std::get_if<TextDelta>(&event)) return !text->text.empty();
    if (const auto* reasoning = std::get_if<ReasoningDelta>(&event)) return !reasoning->text.empty();
    if (std::get_if<ToolCallBegin>(&event)) return true;
    return false;
}

}  // namespace gw::proto
