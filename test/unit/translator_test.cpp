#include "proto/translator.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "http/sse.hpp"
#include "proto/anthropic.hpp"
#include "proto/errors.hpp"

namespace {
using namespace gw::proto;
using J = nlohmann::json;
J request() { return {{"model", "qwen"}, {"messages", J::array({{{"role", "user"}, {"content", "hi"}}})}}; }
TEST(TranslatorTest, SystemAndParallelResultsAndDefaults) {
    auto b = request();
    b["messages"] =
        J::array({{{"role", "system"}, {"content", "one"}},
                  {{"role", "developer"}, {"content", "two"}},
                  {{"role", "assistant"},
                   {"content", nullptr},
                   {"tool_calls", J::array({{{"id", "a"}, {"function", {{"name", "f"}, {"arguments", "bad"}}}}})}},
                  {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "A"}},
                  {{"role", "tool"}, {"tool_call_id", "b"}, {"content", "B"}}});
    b["temperature"] = 1.7;
    auto n = decodeRequest(b, Protocol::openai);
    ASSERT_TRUE(n);
    auto a = encodeRequest(*n, Protocol::anthropic, 123);
    EXPECT_EQ(a["system"], "one\n\ntwo");
    EXPECT_EQ(a["max_tokens"], 123);
    EXPECT_EQ(a["temperature"], 1.0);
    ASSERT_EQ(a["messages"].size(), 2u);
    EXPECT_EQ(a["messages"][1]["content"].size(), 2u);
    EXPECT_EQ(a["messages"][0]["content"][0]["input"], J::object());
    auto back = decodeRequest(a, Protocol::anthropic);
    ASSERT_TRUE(back);
    auto o = encodeRequest(*back, Protocol::openai);
    EXPECT_EQ(o["messages"][1]["content"], nullptr);
    EXPECT_EQ(o["messages"][2]["role"], "tool");
}
TEST(TranslatorTest, ToolChoicesAndUnsupportedRequests) {
    for (auto choice : {J("auto"), J("none"), J("required"), J{{"type", "function"}, {"function", {{"name", "f"}}}}}) {
        auto b = request();
        b["tool_choice"] = choice;
        b["tools"] = J::array({{{"type", "function"}, {"function", {{"name", "f"}, {"parameters", J::object()}}}}});
        auto n = decodeRequest(b, Protocol::openai);
        ASSERT_TRUE(n);
        auto a = encodeRequest(*n, Protocol::anthropic);
        if (choice == "none") {
            EXPECT_FALSE(a.contains("tools"));
            continue;
        }
        auto back = decodeRequest(a, Protocol::anthropic);
        ASSERT_TRUE(back);
        EXPECT_EQ(back->tool_choice, choice);
    }
    auto b = request();
    b["n"] = 2;
    EXPECT_FALSE(decodeRequest(b, Protocol::openai));
    b.erase("n");
    b["response_format"] = {{"type", "json_object"}};
    EXPECT_FALSE(decodeRequest(b, Protocol::openai));
    b.erase("response_format");
    b["messages"][0]["content"] = J::array({{{"type", "image_url"}, {"image_url", {{"url", "x"}}}}});
    EXPECT_FALSE(decodeRequest(b, Protocol::openai));
    b["messages"][0]["content"] = J::array({{{"type", "image"}, {"source", J::object()}}});
    EXPECT_FALSE(decodeRequest(b, Protocol::anthropic));
}
TEST(TranslatorTest, ToolResultsPrecedeUserTextAndThinkingIsDropped) {
    auto b = request();
    b["messages"][0]["content"] = J::array({{{"type", "text"}, {"text", "after"}},
                                            {{"type", "tool_result"},
                                             {"tool_use_id", "t"},
                                             {"is_error", true},
                                             {"content", J::array({{{"type", "text"}, {"text", "failed"}}})}}});
    auto n = decodeRequest(b, Protocol::anthropic);
    ASSERT_TRUE(n);
    auto o = encodeRequest(*n, Protocol::openai);
    EXPECT_EQ(o["messages"][0]["role"], "tool");
    EXPECT_EQ(o["messages"][0]["content"], "[tool error] failed");
    EXPECT_EQ(o["messages"][1]["content"], "after");
}
TEST(TranslatorTest, NonstreamRoundtripAndErrors) {
    J b = {
        {"choices",
         J::array({{{"message",
                     {{"content", "hello"},
                      {"reasoning_content", "think"},
                      {"tool_calls", J::array({{{"id", "t"}, {"function", {{"name", "f"}, {"arguments", "{}"}}}}})}}},
                    {"finish_reason", "tool_calls"}}})},
        {"usage",
         {{"prompt_tokens", 10}, {"completion_tokens", 5}, {"prompt_tokens_details", {{"cached_tokens", 3}}}}}};
    b["choices"][0]["index"] = 0;
    b["choices"][0]["message"]["role"] = "assistant";
    b["choices"][0]["message"]["tool_calls"][0]["type"] = "function";
    auto a = translateResponse(b, Protocol::openai, Protocol::anthropic, "alias", "id");
    EXPECT_EQ(a["usage"]["input_tokens"], 7);
    auto o = translateResponse(a, Protocol::anthropic, Protocol::openai, "alias", "id");
    EXPECT_EQ(o["choices"], b["choices"]);
    EXPECT_EQ(o["usage"]["prompt_tokens"], 10);
    EXPECT_EQ(extractErrorMessage(anthropicErrorBody("exact error", "api_error")), "exact error");
}
struct Semantic {
    std::string text, reason;
    std::map<int, std::string> ids, names, args;
    Usage usage;
    FinishReason finish = FinishReason::error;
};
Semantic collect(const std::vector<StreamEvent>& events) {
    Semantic s;
    for (const auto& e : events) {
        if (auto* v = std::get_if<TextDelta>(&e)) s.text += v->text;
        if (auto* v = std::get_if<ReasoningDelta>(&e)) s.reason += v->text;
        if (auto* v = std::get_if<ToolCallBegin>(&e)) {
            s.ids[v->index] = v->id;
            s.names[v->index] = v->name;
        }
        if (auto* v = std::get_if<ToolCallArgs>(&e)) s.args[v->index] += v->fragment;
        if (auto* v = std::get_if<Usage>(&e)) s.usage = *v;
        if (auto* v = std::get_if<Finish>(&e)) s.finish = v->reason;
    }
    return s;
}
std::vector<StreamEvent> decode(const std::string& wire, Protocol protocol) {
    StreamDecoder decoder(protocol);
    gw::http::SseParser parser;
    std::vector<StreamEvent> all;
    // Deliberately split input at arbitrary byte boundaries.
    for (std::size_t i = 0; i < wire.size(); i += 17)
        parser.feed(std::string_view(wire).substr(i, 17), [&](gw::http::SseEvent&& e) {
            auto events = decoder.feed(e.event, e.data);
            all.insert(all.end(), events.begin(), events.end());
        });
    EXPECT_TRUE(decoder.done());
    return all;
}
TEST(TranslatorTest, RecordedStreamsRoundtrip) {
    for (auto p : {Protocol::openai, Protocol::anthropic}) {
        std::ifstream f(std::string(GW_TEST_DATA_DIR) +
                        (p == Protocol::openai ? "/openai_tools.sse" : "/anthropic_tools.sse"));
        ASSERT_TRUE(f.good());
        std::ostringstream raw;
        raw << f.rdbuf();
        auto events = decode(raw.str(), p);
        auto original = collect(events);
        ASSERT_GE(original.ids.size(), 2u);
        ASSERT_GT(original.usage.prompt, 0);
        ASSERT_GT(original.usage.completion, 0);
        auto other = p == Protocol::openai ? Protocol::anthropic : Protocol::openai;
        StreamEncoder encoder(other, "alias", "roundtrip", true);
        encoder.initialUsage(original.usage);
        std::string wire;
        for (const auto& e : events) wire += encoder.feed(e);
        auto result = collect(decode(wire, other));
        EXPECT_EQ(result.text, original.text);
        EXPECT_EQ(result.reason, original.reason);
        EXPECT_EQ(result.ids, original.ids);
        EXPECT_EQ(result.names, original.names);
        EXPECT_EQ(result.args, original.args);
        EXPECT_EQ(result.finish, original.finish);
        EXPECT_EQ(result.usage.prompt, original.usage.prompt);
        EXPECT_EQ(result.usage.cached, original.usage.cached);
        EXPECT_EQ(result.usage.completion, original.usage.completion);
        for (const auto& [index, args] : result.args) {
            (void)index;
            EXPECT_FALSE(J::parse(args, nullptr, false).is_discarded());
        }
    }
}
TEST(TranslatorTest, InterleavedToolsKeepBlocksValidAndTerminalIsFinal) {
    StreamEncoder e(Protocol::anthropic, "m", "i", true);
    std::string wire;
    std::vector<StreamEvent> events = {ToolCallBegin{0, "a", "f"},
                                       ToolCallBegin{1, "b", "f"},
                                       ToolCallArgs{1, "{\"b\":"},
                                       ToolCallArgs{0, "{}"},
                                       ToolCallArgs{1, "2}"},
                                       ToolCallEnd{0},
                                       ToolCallEnd{1},
                                       Usage{10, 5, 2},
                                       Finish{FinishReason::tool_calls, "tool_calls"}};
    for (auto& v : events) {
        wire += e.feed(v);
    }
    EXPECT_TRUE(e.feed(TextDelta{"late"}).empty());
    auto got = collect(decode(wire, Protocol::anthropic));
    EXPECT_EQ(got.args[0], "{}");
    EXPECT_EQ(got.args[1], "{\"b\":2}");
    int open = -1;
    gw::http::SseParser parser;
    parser.feed(wire, [&](gw::http::SseEvent&& s) {
        auto j = J::parse(s.data);
        auto type = j.value("type", "");
        if (type == "content_block_start") {
            EXPECT_EQ(open, -1);
            open = j["index"];
        }
        if (type == "content_block_delta") {
            EXPECT_EQ(open, j["index"]);
        }
        if (type == "content_block_stop") {
            EXPECT_EQ(open, j["index"]);
            open = -1;
        }
    });
    EXPECT_EQ(open, -1);
}
TEST(TranslatorTest, MalformedToolChoiceIsARequestError) {
    for (auto choice : {J::object(), J{{"type", "function"}}, J("invalid")}) {
        auto b = request();
        b["tool_choice"] = choice;
        EXPECT_FALSE(decodeRequest(b, Protocol::openai));
    }
}
TEST(TranslatorTest, PassthroughTruncationClosesActualBlocksOnce) {
    AnthropicStreamState state;
    state.observe({{"type", "message_start"}});
    state.observe({{"type", "content_block_start"}, {"index", 0}});
    state.observe({{"type", "content_block_stop"}, {"index", 0}});
    state.observe({{"type", "content_block_start"}, {"index", 3}});
    auto wire = state.finish({12, 7, 2});
    EXPECT_EQ(wire.find("message_start"), std::string::npos);
    EXPECT_NE(wire.find("\"index\":3"), std::string::npos);
    EXPECT_EQ(wire.find("\"index\":0"), std::string::npos);
    EXPECT_TRUE(state.finish({12, 7, 2}).empty());
}
TEST(TranslatorTest, ErrorHasNoSuccessTerminator) {
    StreamEncoder e(Protocol::anthropic, "m", "i", true);
    auto wire = e.feed(Finish{FinishReason::error, "broken"});
    EXPECT_NE(wire.find("event: error"), std::string::npos);
    EXPECT_EQ(wire.find("message_stop"), std::string::npos);
    StreamDecoder d(Protocol::openai);
    auto events = d.feed("", "{\"error\":{\"message\":\"broken\"}}");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(std::get<Finish>(events[0]).reason, FinishReason::error);
    EXPECT_TRUE(d.feed("", "[DONE]").empty());
}
}  // namespace
