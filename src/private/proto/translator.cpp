#include "proto/translator.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <stdexcept>

namespace gw::proto {
namespace {
using J = nlohmann::json;
std::string text(const J& value) {
    if (value.is_null()) return {};
    if (value.is_string()) return value.get<std::string>();
    if (!value.is_array()) throw std::runtime_error("content must be text or text blocks");
    std::string out;
    for (const auto& part : value) {
        if (part.value("type", "") != "text")
            throw std::runtime_error("non-text content is not supported across protocols");
        if (!out.empty()) out += '\n';
        out += part.at("text").get<std::string>();
    }
    return out;
}
J parseArgs(const std::string& args) {
    auto j = J::parse(args, nullptr, false);
    return j.is_discarded() ? J::object() : j;
}
std::string frame(const J& body, bool anthropic = false) {
    return (anthropic ? "event: " + body.at("type").get<std::string>() + "\n" : "") + "data: " + body.dump() + "\n\n";
}
J anthUsage(const Usage& u) {
    return {
        {"input_tokens", u.prompt - u.cached}, {"cache_read_input_tokens", u.cached}, {"output_tokens", u.completion}};
}
J openUsage(const Usage& u) {
    return {{"prompt_tokens", u.prompt},
            {"completion_tokens", u.completion},
            {"total_tokens", u.prompt + u.completion},
            {"prompt_tokens_details", {{"cached_tokens", u.cached}}}};
}
void readUsage(Usage& u, const J& j, Protocol protocol) {
    if (protocol == Protocol::openai) {
        u.prompt = j.value("prompt_tokens", u.prompt);
        u.completion = j.value("completion_tokens", u.completion);
        if (j.contains("prompt_tokens_details") && j["prompt_tokens_details"].is_object())
            u.cached = j["prompt_tokens_details"].value("cached_tokens", u.cached);
    } else {
        u.cached = j.value("cache_read_input_tokens", u.cached);
        if (j.contains("input_tokens"))
            u.prompt = j.at("input_tokens").get<std::int64_t>() + u.cached +
                       j.value("cache_creation_input_tokens", std::int64_t{0});
        u.completion = j.value("output_tokens", u.completion);
    }
}
}  // namespace
std::expected<ChatRequest, std::string> decodeRequest(const J& body, Protocol protocol) {
    try {
        ChatRequest r;
        r.model = body.at("model").get<std::string>();
        r.stream = body.value("stream", false);
        r.client_wants_usage =
            protocol == Protocol::anthropic ||
            (body.contains("stream_options") && body["stream_options"].value("include_usage", false));
        if (body.contains("max_completion_tokens"))
            r.max_tokens = body.at("max_completion_tokens").get<int>();
        else if (body.contains("max_tokens"))
            r.max_tokens = body.at("max_tokens").get<int>();
        if (body.contains("temperature")) r.temperature = body.at("temperature").get<double>();
        if (body.contains("top_p")) r.top_p = body.at("top_p").get<double>();
        const char* stop = protocol == Protocol::openai ? "stop" : "stop_sequences";
        if (body.contains(stop) && !body[stop].is_null()) {
            if (body[stop].is_string())
                r.stop.push_back(body[stop].get<std::string>());
            else
                r.stop = body[stop].get<std::vector<std::string>>();
        }
        if (protocol == Protocol::openai) {
            if (body.value("n", 1) > 1) throw std::runtime_error("n > 1 is not supported across protocols");
            if (body.contains("response_format") && body["response_format"].value("type", "text") != "text")
                throw std::runtime_error("response_format is not supported across protocols");
            r.user = body.value("user", "");
        } else {
            if (body.contains("metadata") && body["metadata"].is_object())
                r.user = body["metadata"].value("user_id", "");
            if (body.contains("system")) {
                Message m;
                m.role = Role::system;
                m.content = text(body["system"]);
                r.messages.push_back(std::move(m));
            }
        }
        for (const auto& item : body.at("messages")) {
            Message m;
            const auto role = item.at("role").get<std::string>();
            if (role == "system" || role == "developer")
                m.role = Role::system;
            else if (role == "assistant")
                m.role = Role::assistant;
            else if (role == "tool")
                m.role = Role::tool;
            else if (role != "user")
                throw std::runtime_error("unsupported message role: " + role);
            if (protocol == Protocol::openai) {
                m.content = text(item.value("content", J(nullptr)));
                if (m.role == Role::tool) m.tool_call_id = item.at("tool_call_id").get<std::string>();
                for (const auto& t : item.value("tool_calls", J::array())) {
                    const auto& f = t.at("function");
                    m.tool_calls.push_back({t.at("id").get<std::string>(), f.at("name").get<std::string>(),
                                            f.at("arguments").get<std::string>()});
                }
            } else if (item.at("content").is_string())
                m.content = item.at("content").get<std::string>();
            else {
                for (const auto& c : item.at("content")) {
                    const auto type = c.at("type").get<std::string>();
                    if (type == "text") {
                        if (!m.content.empty()) m.content += '\n';
                        m.content += c.at("text").get<std::string>();
                    } else if (type == "tool_use")
                        m.tool_calls.push_back(
                            {c.at("id").get<std::string>(), c.at("name").get<std::string>(), c.at("input").dump()});
                    else if (type == "tool_result") {
                        Message result;
                        result.role = Role::tool;
                        result.tool_call_id = c.at("tool_use_id").get<std::string>();
                        result.content =
                            (c.value("is_error", false) ? "[tool error] " : "") + text(c.value("content", J("")));
                        r.messages.push_back(std::move(result));
                    } else if (type != "thinking" && type != "redacted_thinking")
                        throw std::runtime_error("unsupported content block: " + type);
                }
                if (m.content.empty() && m.tool_calls.empty()) continue;
            }
            r.messages.push_back(std::move(m));
        }
        for (const auto& t : body.value("tools", J::array())) {
            const auto& f = protocol == Protocol::openai ? t.at("function") : t;
            r.tools.push_back({f.at("name").get<std::string>(), f.value("description", ""),
                               f.value(protocol == Protocol::openai ? "parameters" : "input_schema", J::object())});
        }
        if (body.contains("tool_choice")) {
            r.tool_choice = body["tool_choice"];
            if (protocol == Protocol::anthropic) {
                auto type = r.tool_choice.at("type").get<std::string>();
                if (type == "tool")
                    r.tool_choice = {{"type", "function"}, {"function", {{"name", r.tool_choice.at("name")}}}};
                else
                    r.tool_choice = type == "any" ? "required" : type;
            }
        }
        if (!r.tool_choice.is_null()) {
            if (r.tool_choice.is_string()) {
                auto choice = r.tool_choice.get<std::string>();
                if (choice != "auto" && choice != "none" && choice != "required")
                    throw std::runtime_error("invalid tool_choice");
            } else if (!r.tool_choice.is_object() || r.tool_choice.value("type", "") != "function" ||
                       !r.tool_choice.contains("function") || !r.tool_choice["function"].is_object() ||
                       !r.tool_choice["function"].contains("name") || !r.tool_choice["function"]["name"].is_string()) {
                throw std::runtime_error("invalid tool_choice function name");
            }
        }
        return r;
    } catch (const std::exception& e) {
        return std::unexpected(e.what());
    }
}

J encodeRequest(const ChatRequest& r, Protocol protocol, int default_max_tokens) {
    J out = {{"model", r.model}, {"messages", J::array()}, {"stream", r.stream}};
    if (r.max_tokens || protocol == Protocol::anthropic) out["max_tokens"] = r.max_tokens.value_or(default_max_tokens);
    if (r.temperature)
        out["temperature"] = protocol == Protocol::anthropic ? std::clamp(*r.temperature, 0.0, 1.0) : *r.temperature;
    if (r.top_p) out["top_p"] = *r.top_p;
    if (!r.stop.empty()) out[protocol == Protocol::openai ? "stop" : "stop_sequences"] = r.stop;
    if (!r.user.empty()) {
        if (protocol == Protocol::openai)
            out["user"] = r.user;
        else
            out["metadata"]["user_id"] = r.user;
    }
    std::string system;
    for (const auto& m : r.messages) {
        if (protocol == Protocol::openai) {
            std::string role = m.role == Role::system      ? "system"
                               : m.role == Role::assistant ? "assistant"
                               : m.role == Role::tool      ? "tool"
                                                           : "user";
            J j = {{"role", role}, {"content", m.content.empty() && !m.tool_calls.empty() ? J(nullptr) : J(m.content)}};
            if (m.role == Role::tool) j["tool_call_id"] = m.tool_call_id;
            for (const auto& t : m.tool_calls)
                j["tool_calls"].push_back(
                    {{"id", t.id}, {"type", "function"}, {"function", {{"name", t.name}, {"arguments", t.arguments}}}});
            out["messages"].push_back(std::move(j));
        } else {
            if (m.role == Role::system) {
                if (!system.empty()) system += "\n\n";
                system += m.content;
                continue;
            }
            std::string role = m.role == Role::assistant ? "assistant" : "user";
            J blocks = J::array();
            if (m.role == Role::tool)
                blocks.push_back({{"type", "tool_result"}, {"tool_use_id", m.tool_call_id}, {"content", m.content}});
            else {
                if (!m.content.empty()) blocks.push_back({{"type", "text"}, {"text", m.content}});
                for (const auto& t : m.tool_calls)
                    blocks.push_back(
                        {{"type", "tool_use"}, {"id", t.id}, {"name", t.name}, {"input", parseArgs(t.arguments)}});
            }
            auto& messages = out["messages"];
            if (!messages.empty() && messages.back()["role"] == role)
                for (auto& b : blocks) messages.back()["content"].push_back(std::move(b));
            else
                messages.push_back({{"role", role}, {"content", std::move(blocks)}});
        }
    }
    if (!system.empty()) out["system"] = system;
    bool no_tools = r.tool_choice.is_string() && r.tool_choice == "none" && protocol == Protocol::anthropic;
    if (!no_tools) {
        for (const auto& t : r.tools) {
            if (protocol == Protocol::openai)
                out["tools"].push_back(
                    {{"type", "function"},
                     {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
            else
                out["tools"].push_back(
                    {{"name", t.name}, {"description", t.description}, {"input_schema", t.parameters}});
        }
        if (!r.tool_choice.is_null()) {
            if (protocol == Protocol::openai)
                out["tool_choice"] = r.tool_choice;
            else if (r.tool_choice.is_string())
                out["tool_choice"] = {{"type", r.tool_choice == "required" ? "any" : "auto"}};
            else
                out["tool_choice"] = {{"type", "tool"}, {"name", r.tool_choice.at("function").at("name")}};
        }
    }
    if (r.stream && protocol == Protocol::openai) out["stream_options"]["include_usage"] = true;
    return out;
}

FinishReason decodeFinish(std::string_view r, Protocol p) {
    if (r == "stop" || (p == Protocol::anthropic && (r == "end_turn" || r == "stop_sequence")))
        return FinishReason::stop;
    if (r == "length" || r == "max_tokens") return FinishReason::length;
    if (r == "tool_calls" || r == "function_call" || r == "tool_use") return FinishReason::tool_calls;
    if (r == "content_filter" || r == "refusal") return FinishReason::content_filter;
    return FinishReason::error;
}
std::string encodeFinish(FinishReason r, Protocol p) {
    switch (r) {
        case FinishReason::stop:
            return p == Protocol::openai ? "stop" : "end_turn";
        case FinishReason::length:
            return p == Protocol::openai ? "length" : "max_tokens";
        case FinishReason::tool_calls:
            return p == Protocol::openai ? "tool_calls" : "tool_use";
        case FinishReason::content_filter:
            return p == Protocol::openai ? "content_filter" : "refusal";
        case FinishReason::error:
            return "error";
    }
    return "error";
}

J translateResponse(const J& b, Protocol from, Protocol to, const std::string& model, const std::string& id) {
    if (from == to) return b;
    Usage u;
    if (b.contains("usage")) readUsage(u, b["usage"], from);
    J message;
    std::string reason;
    if (from == Protocol::openai) {
        const auto& c = b.at("choices").at(0);
        const auto& m = c.at("message");
        J content = J::array();
        if (m.contains("reasoning_content") && !m["reasoning_content"].is_null() && m["reasoning_content"] != "")
            content.push_back({{"type", "thinking"}, {"thinking", m["reasoning_content"]}, {"signature", ""}});
        if (m.contains("content") && !m["content"].is_null() && m["content"] != "")
            content.push_back({{"type", "text"}, {"text", m["content"]}});
        for (const auto& t : m.value("tool_calls", J::array()))
            content.push_back({{"type", "tool_use"},
                               {"id", t.at("id")},
                               {"name", t.at("function").at("name")},
                               {"input", parseArgs(t.at("function").at("arguments").get<std::string>())}});
        return {{"id", "msg_" + id},
                {"type", "message"},
                {"role", "assistant"},
                {"model", model},
                {"content", content},
                {"stop_reason", encodeFinish(decodeFinish(c.value("finish_reason", ""), from), to)},
                {"stop_sequence", nullptr},
                {"usage", anthUsage(u)}};
    }
    message = {{"role", "assistant"}, {"content", nullptr}};
    std::string content, reasoning;
    for (const auto& c : b.at("content")) {
        const auto type = c.value("type", "");
        if (type == "text") content += c.at("text").get<std::string>();
        if (type == "thinking") reasoning += c.at("thinking").get<std::string>();
        if (type == "tool_use")
            message["tool_calls"].push_back(
                {{"id", c.at("id")},
                 {"type", "function"},
                 {"function", {{"name", c.at("name")}, {"arguments", c.at("input").dump()}}}});
    }
    if (!content.empty()) message["content"] = content;
    if (!reasoning.empty()) message["reasoning_content"] = reasoning;
    return {
        {"id", "chatcmpl-" + id},
        {"object", "chat.completion"},
        {"created", std::time(nullptr)},
        {"model", model},
        {"choices", J::array({{{"index", 0},
                               {"message", message},
                               {"finish_reason", encodeFinish(decodeFinish(b.value("stop_reason", ""), from), to)}}})},
        {"usage", openUsage(u)}};
}

std::vector<StreamEvent> StreamDecoder::feed(std::string_view event, std::string_view data) {
    std::vector<StreamEvent> out;
    if (done_) return out;
    try {
        bool terminal = protocol_ == Protocol::openai && data == "[DONE]";
        J b;
        if (!terminal) {
            b = J::parse(data);
            if (b.contains("error")) {
                done_ = true;
                out.push_back(Finish{FinishReason::error, b["error"].value("message", "upstream error")});
                return out;
            }
        }
        if (protocol_ == Protocol::openai && !terminal) {
            if (b.contains("usage") && b["usage"].is_object()) readUsage(usage_, b["usage"], protocol_);
            const auto choices = b.value("choices", J::array());
            if (!choices.empty()) {
                const auto& c = choices[0];
                if (c.contains("finish_reason") && c["finish_reason"].is_string())
                    reason_ = c["finish_reason"].get<std::string>();
                const auto d = c.value("delta", J::object());
                if (d.contains("content") && d["content"].is_string() && d["content"] != "")
                    out.push_back(TextDelta{d["content"].get<std::string>()});
                const auto reasoning = d.value("reasoning_content", d.value("reasoning", J(nullptr)));
                if (reasoning.is_string() && reasoning != "")
                    out.push_back(ReasoningDelta{reasoning.get<std::string>()});
                for (const auto& t : d.value("tool_calls", J::array())) {
                    int key = t.value("index", 0);
                    const auto f = t.value("function", J::object());
                    auto [it, inserted] = tools_.try_emplace(key, Tool{static_cast<int>(tools_.size())});
                    if (inserted)
                        out.push_back(ToolCallBegin{it->second.index, t.value("id", ""), f.value("name", "")});
                    const auto args = f.value("arguments", "");
                    if (!args.empty()) {
                        out.push_back(ToolCallArgs{it->second.index, args});
                        it->second.args = true;
                    }
                }
            }
        } else if (protocol_ == Protocol::anthropic) {
            std::string type = b.value("type", std::string(event));
            if (type == "message_start") readUsage(usage_, b.at("message").at("usage"), protocol_);
            if (type == "message_delta") {
                if (b.contains("usage")) readUsage(usage_, b["usage"], protocol_);
                if (b.contains("delta") && b["delta"].contains("stop_reason") && b["delta"]["stop_reason"].is_string())
                    reason_ = b["delta"]["stop_reason"].get<std::string>();
            }
            if (type == "content_block_start") {
                const auto& c = b.at("content_block");
                const auto t = c.value("type", "");
                if (t == "tool_use") {
                    int index = static_cast<int>(tools_.size());
                    auto& tool = tools_.emplace(b.at("index").get<int>(), Tool{index}).first->second;
                    out.push_back(ToolCallBegin{index, c.at("id").get<std::string>(), c.at("name").get<std::string>()});
                    if (c.contains("input") && !c["input"].empty()) {
                        out.push_back(ToolCallArgs{index, c["input"].dump()});
                        tool.args = true;
                    }
                } else if (t == "text" && c.value("text", "") != "")
                    out.push_back(TextDelta{c["text"].get<std::string>()});
                else if (t == "thinking" && c.value("thinking", "") != "")
                    out.push_back(ReasoningDelta{c["thinking"].get<std::string>()});
            }
            if (type == "content_block_delta") {
                const auto& d = b.at("delta");
                const auto t = d.value("type", "");
                if (t == "text_delta") out.push_back(TextDelta{d.at("text").get<std::string>()});
                if (t == "thinking_delta") out.push_back(ReasoningDelta{d.at("thinking").get<std::string>()});
                if (t == "input_json_delta") {
                    auto& tool = tools_.at(b.at("index").get<int>());
                    tool.args = true;
                    out.push_back(ToolCallArgs{tool.index, d.at("partial_json").get<std::string>()});
                }
            }
            if (type == "content_block_stop") {
                auto it = tools_.find(b.at("index").get<int>());
                if (it != tools_.end() && !it->second.ended) {
                    if (!it->second.args) out.push_back(ToolCallArgs{it->second.index, "{}"});
                    out.push_back(ToolCallEnd{it->second.index});
                    it->second.ended = true;
                }
            }
            terminal = type == "message_stop";
        }
        if (terminal) {
            for (auto& [key, t] : tools_) {
                (void)key;
                if (t.ended) continue;
                if (!t.args) out.push_back(ToolCallArgs{t.index, "{}"});
                out.push_back(ToolCallEnd{t.index});
                t.ended = true;
            }
            out.push_back(usage_);
            out.push_back(Finish{decodeFinish(reason_, protocol_), reason_});
            done_ = true;
        }
    } catch (const std::exception& e) {
        done_ = true;
        out.push_back(Finish{FinishReason::error, std::string("invalid upstream SSE: ") + e.what()});
    }
    return out;
}

StreamEncoder::StreamEncoder(Protocol p, std::string model, std::string id, bool wants)
    : protocol_(p), model_(std::move(model)), id_(std::move(id)), created_(std::time(nullptr)), wants_usage_(wants) {}
J StreamEncoder::chunk(J delta, J finish) const {
    return {{"id", "chatcmpl-" + id_},
            {"object", "chat.completion.chunk"},
            {"created", created_},
            {"model", model_},
            {"choices", J::array({{{"index", 0}, {"delta", std::move(delta)}, {"finish_reason", std::move(finish)}}})}};
}
void StreamEncoder::start(std::string& out) {
    if (started_) return;
    started_ = true;
    if (protocol_ == Protocol::openai)
        out += frame(chunk({{"role", "assistant"}, {"content", ""}}));
    else {
        auto u = usage_;
        u.completion = 0;
        out += frame({{"type", "message_start"},
                      {"message",
                       {{"id", "msg_" + id_},
                        {"type", "message"},
                        {"role", "assistant"},
                        {"model", model_},
                        {"content", J::array()},
                        {"stop_reason", nullptr},
                        {"stop_sequence", nullptr},
                        {"usage", anthUsage(u)}}}},
                     true);
    }
}
void StreamEncoder::closeBlock(std::string& out) {
    if (block_ < 0) return;
    out += frame({{"type", "content_block_stop"}, {"index", block_}}, true);
    block_ = -1;
    block_type_.clear();
    active_tool_ = -1;
}
void StreamEncoder::textBlock(std::string& out, bool thinking, const std::string& value) {
    const std::string type = thinking ? "thinking" : "text";
    if (block_type_ != type) {
        closeBlock(out);
        block_ = next_block_++;
        block_type_ = type;
        out +=
            frame({{"type", "content_block_start"}, {"index", block_}, {"content_block", {{"type", type}, {type, ""}}}},
                  true);
    }
    out += frame(
        {{"type", "content_block_delta"}, {"index", block_}, {"delta", {{"type", type + "_delta"}, {type, value}}}},
        true);
}
void StreamEncoder::emitTool(std::string& out, int index) {
    auto& t = tools_.at(index);
    if (t.block >= 0) return;
    closeBlock(out);
    block_ = next_block_++;
    block_type_ = "tool_use";
    active_tool_ = index;
    t.block = block_;
    out += frame({{"type", "content_block_start"},
                  {"index", block_},
                  {"content_block", {{"type", "tool_use"}, {"id", t.id}, {"name", t.name}, {"input", J::object()}}}},
                 true);
    if (!t.args.empty())
        out += frame({{"type", "content_block_delta"},
                      {"index", block_},
                      {"delta", {{"type", "input_json_delta"}, {"partial_json", t.args}}}},
                     true);
    t.args.clear();
    if (t.ended) closeBlock(out);
}
std::string StreamEncoder::feed(const StreamEvent& e) {
    if (done_) return {};
    std::string out;
    if (const auto* u = std::get_if<Usage>(&e)) {
        usage_ = *u;
        return out;
    }
    if (const auto* f = std::get_if<Finish>(&e); f && f->reason == FinishReason::error) {
        done_ = true;
        if (protocol_ == Protocol::openai) return frame({{"error", {{"type", "upstream_error"}, {"message", f->raw}}}});
        return frame({{"type", "error"}, {"error", {{"type", "api_error"}, {"message", f->raw}}}}, true);
    }
    start(out);
    if (const auto* t = std::get_if<TextDelta>(&e)) {
        if (protocol_ == Protocol::openai)
            out += frame(chunk({{"content", t->text}}));
        else if (active_tool_ >= 0)
            deferred_.push_back(e);
        else
            textBlock(out, false, t->text);
    } else if (const auto* r = std::get_if<ReasoningDelta>(&e)) {
        if (protocol_ == Protocol::openai)
            out += frame(chunk({{"reasoning_content", r->text}}));
        else if (active_tool_ >= 0)
            deferred_.push_back(e);
        else
            textBlock(out, true, r->text);
    } else if (const auto* begin = std::get_if<ToolCallBegin>(&e)) {
        tools_[begin->index] = Tool{begin->id, begin->name, {}, false, -1};
        if (protocol_ == Protocol::openai)
            out +=
                frame(chunk({{"tool_calls", J::array({{{"index", begin->index},
                                                       {"id", begin->id},
                                                       {"type", "function"},
                                                       {"function", {{"name", begin->name}, {"arguments", ""}}}}})}}));
        else if (active_tool_ < 0)
            emitTool(out, begin->index);
    } else if (const auto* args = std::get_if<ToolCallArgs>(&e)) {
        if (protocol_ == Protocol::openai)
            out += frame(chunk(
                {{"tool_calls", J::array({{{"index", args->index}, {"function", {{"arguments", args->fragment}}}}})}}));
        else if (active_tool_ == args->index)
            out += frame({{"type", "content_block_delta"},
                          {"index", block_},
                          {"delta", {{"type", "input_json_delta"}, {"partial_json", args->fragment}}}},
                         true);
        else {
            auto& buffer = tools_.at(args->index).args;
            if (buffer.size() + args->fragment.size() > 4 * 1024 * 1024)
                throw std::runtime_error("interleaved tool arguments too large");
            buffer += args->fragment;
        }
    } else if (const auto* end = std::get_if<ToolCallEnd>(&e)) {
        tools_.at(end->index).ended = true;
        if (protocol_ == Protocol::anthropic) {
            if (active_tool_ == end->index) closeBlock(out);
            if (active_tool_ < 0)
                for (auto& [index, t] : tools_)
                    if (t.block < 0 && t.ended) emitTool(out, index);
        }
    } else if (const auto* f = std::get_if<Finish>(&e)) {
        if (protocol_ == Protocol::openai) {
            out += frame(chunk(J::object(), encodeFinish(f->reason, protocol_)));
            if (wants_usage_) {
                auto j = chunk(J::object());
                j["choices"] = J::array();
                j["usage"] = openUsage(usage_);
                out += frame(j);
            }
            out += "data: [DONE]\n\n";
        } else {
            closeBlock(out);
            for (auto& [index, t] : tools_)
                if (t.block < 0) {
                    t.ended = true;
                    emitTool(out, index);
                }
            auto deferred = std::move(deferred_);
            for (const auto& d : deferred) out += feed(d);
            closeBlock(out);
            out += frame({{"type", "message_delta"},
                          {"delta", {{"stop_reason", encodeFinish(f->reason, protocol_)}, {"stop_sequence", nullptr}}},
                          {"usage", anthUsage(usage_)}},
                         true);
            out += frame({{"type", "message_stop"}}, true);
        }
        done_ = true;
    }
    return out;
}
}  // namespace gw::proto
