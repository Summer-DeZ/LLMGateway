#include "http/sse.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using gw::http::SseEvent;
using gw::http::SseParser;

namespace {

std::vector<SseEvent> parseAll(const std::string& text) {
    SseParser parser;
    std::vector<SseEvent> events;
    parser.feed(text, [&](SseEvent&& event) { events.push_back(std::move(event)); });
    return events;
}

}  // namespace

TEST(SseTest, SimpleEvent) {
    auto events = parseAll("data: hello\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "hello");
    EXPECT_EQ(events[0].event, "");
}

TEST(SseTest, CrLfAndLoneCr) {
    auto events = parseAll("data: a\r\n\r\ndata: b\r\rdata: c\n\n");
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].data, "a");
    EXPECT_EQ(events[1].data, "b");
    EXPECT_EQ(events[2].data, "c");
}

TEST(SseTest, BomStripped) {
    std::string text = "\xEF\xBB\xBF" "data: x\n\n";
    auto events = parseAll(text);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "x");
}

TEST(SseTest, CommentsIgnored) {
    auto events = parseAll(": keep-alive\n\ndata: y\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "y");
}

TEST(SseTest, MultiLineData) {
    auto events = parseAll("data: line1\ndata: line2\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "line1\nline2");
}

TEST(SseTest, EventNameAndId) {
    auto events = parseAll("event: message_start\nid: 42\ndata: {}\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].event, "message_start");
    EXPECT_EQ(events[0].id, "42");
    EXPECT_EQ(events[0].data, "{}");
}

TEST(SseTest, FieldWithoutColon) {
    // 与 DAgent 一致：data 字段出现过（即使值为空）就在空行派发
    auto events = parseAll("data\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "");

    // 只有注释/空行的流不派发
    auto none = parseAll(": ping\n\n\n");
    EXPECT_TRUE(none.empty());
}

TEST(SseTest, UnfinishedEventDroppedOnReset) {
    SseParser parser;
    std::vector<SseEvent> events;
    parser.feed("data: partial\n", [&](SseEvent&& e) { events.push_back(std::move(e)); });
    parser.reset();
    EXPECT_TRUE(events.empty());
}

TEST(SseTest, ByteByByteEquivalence) {
    std::string text =
        "\xEF\xBB\xBF" "event: a\ndata: 1\ndata: 2\n\n: c\r\n\ndata: [DONE]\n\n";
    auto whole = parseAll(text);
    SseParser parser;
    std::vector<SseEvent> bytewise;
    for (char c : text) {
        std::string one(1, c);
        parser.feed(one, [&](SseEvent&& e) { bytewise.push_back(std::move(e)); });
    }
    ASSERT_EQ(whole.size(), bytewise.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
        EXPECT_EQ(whole[i].data, bytewise[i].data);
        EXPECT_EQ(whole[i].event, bytewise[i].event);
    }
}

TEST(SseTest, BytesSinceLastEvent) {
    SseParser parser;
    int events = 0;
    parser.feed("data: a\n\n", [&](SseEvent&&) { ++events; });
    EXPECT_EQ(events, 1);
    EXPECT_EQ(parser.bytesSinceLastEvent(), 0u);
    parser.feed("data: b", [&](SseEvent&&) { ++events; });
    EXPECT_EQ(parser.bytesSinceLastEvent(), 7u);
}
