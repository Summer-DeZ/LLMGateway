#include "http/response_parser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "net/buffer.hpp"

using gw::http::Headers;
using gw::http::ResponseParser;
using gw::http::ResponseParserLimits;
using gw::net::Buffer;

namespace {

struct Recorder {
    std::vector<std::pair<int, Headers>> heads;
    std::string body;
    bool completed = false;
    bool reusable = false;
    std::string error;

    ResponseParser::Callbacks callbacks() {
        return ResponseParser::Callbacks{
            .on_head = [this](int status, const Headers& h) { heads.emplace_back(status, h); },
            .on_data = [this](std::string_view d) { body.append(d); },
            .on_complete = [this](bool r) {
                completed = true;
                reusable = r;
            },
            .on_error = [this](std::string e) { error = std::move(e); }};
    }
};

}  // namespace

TEST(ResponseParserTest, ContentLength) {
    Recorder rec;
    ResponseParser parser(ResponseParserLimits{}, rec.callbacks());
    Buffer buf;
    buf.append("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello");
    parser.feed(buf);
    EXPECT_TRUE(rec.completed);
    EXPECT_TRUE(rec.reusable);
    ASSERT_EQ(rec.heads.size(), 1u);
    EXPECT_EQ(rec.heads[0].first, 200);
    EXPECT_EQ(rec.body, "hello");
}

TEST(ResponseParserTest, Chunked) {
    Recorder rec;
    ResponseParser parser(ResponseParserLimits{}, rec.callbacks());
    Buffer buf;
    buf.append("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    parser.feed(buf);
    EXPECT_TRUE(rec.completed);
    EXPECT_TRUE(rec.reusable);
    EXPECT_EQ(rec.body, "Wikipedia");
}

TEST(ResponseParserTest, UntilClose) {
    Recorder rec;
    ResponseParser parser(ResponseParserLimits{}, rec.callbacks());
    Buffer buf;
    buf.append("HTTP/1.1 200 OK\r\n\r\npartial");
    parser.feed(buf);
    EXPECT_FALSE(rec.completed);
    EXPECT_EQ(rec.body, "partial");
    parser.feedEof();
    EXPECT_TRUE(rec.completed);
    EXPECT_FALSE(rec.reusable);
}

TEST(ResponseParserTest, ByteByByteEquivalence) {
    std::string raw =
        "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nX-A: 1\r\n\r\nhello world";
    Recorder whole_rec;
    ResponseParser whole_parser(ResponseParserLimits{}, whole_rec.callbacks());
    Buffer whole_buf;
    whole_buf.append(raw);
    whole_parser.feed(whole_buf);

    Recorder byte_rec;
    ResponseParser byte_parser(ResponseParserLimits{}, byte_rec.callbacks());
    Buffer byte_buf;
    for (char c : raw) {
        byte_buf.append(&c, 1);
        byte_parser.feed(byte_buf);
    }
    EXPECT_EQ(whole_rec.body, byte_rec.body);
    EXPECT_EQ(whole_rec.completed, byte_rec.completed);
    EXPECT_EQ(whole_rec.heads.size(), byte_rec.heads.size());
}

TEST(ResponseParserTest, Skips100Continue) {
    Recorder rec;
    ResponseParser parser(ResponseParserLimits{}, rec.callbacks());
    Buffer buf;
    buf.append("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    parser.feed(buf);
    ASSERT_EQ(rec.heads.size(), 1u);
    EXPECT_EQ(rec.heads[0].first, 200);
    EXPECT_EQ(rec.body, "ok");
}

TEST(ResponseParserTest, ErrorBodyTruncated) {
    Recorder rec;
    ResponseParserLimits limits;
    limits.max_error_body_bytes = 10;
    ResponseParser parser(limits, rec.callbacks());
    Buffer buf;
    std::string body(100, 'e');
    buf.append("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 100\r\n\r\n" + body);
    parser.feed(buf);
    EXPECT_TRUE(rec.completed);
    EXPECT_EQ(rec.body.size(), 10u);
    EXPECT_FALSE(rec.reusable);
}

TEST(ResponseParserTest, ClosedMidBody) {
    Recorder rec;
    ResponseParser parser(ResponseParserLimits{}, rec.callbacks());
    Buffer buf;
    buf.append("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
    parser.feed(buf);
    EXPECT_FALSE(rec.completed);
    parser.feedEof();
    EXPECT_FALSE(rec.error.empty());
    EXPECT_FALSE(rec.completed);
}

TEST(ResponseParserTest, HeadTooLarge) {
    Recorder rec;
    ResponseParserLimits limits;
    limits.max_head_bytes = 128;
    ResponseParser parser(limits, rec.callbacks());
    Buffer buf;
    buf.append("HTTP/1.1 200 OK\r\nX-Big: " + std::string(1000, 'a'));
    parser.feed(buf);
    EXPECT_FALSE(rec.error.empty());
}
