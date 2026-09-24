#include "http/request_parser.hpp"

#include <gtest/gtest.h>

#include <string>

#include "net/buffer.hpp"

using gw::http::ParserLimits;
using gw::http::RequestParser;
using gw::net::Buffer;

namespace {

RequestParser::Result feedAll(RequestParser& parser, Buffer& buf, const std::string& data) {
    buf.append(data);
    return parser.feed(buf);
}

}  // namespace

TEST(RequestParserTest, CompleteRequest) {
    RequestParser parser;
    Buffer buf;
    std::string raw =
        "POST /v1/chat/completions?x=1 HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 7\r\n"
        "\r\n"
        "{\"a\":1}";
    EXPECT_EQ(feedAll(parser, buf, raw), RequestParser::Result::complete);
    auto req = parser.take();
    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(req.target, "/v1/chat/completions?x=1");
    EXPECT_EQ(req.path, "/v1/chat/completions");
    EXPECT_EQ(req.query, "x=1");
    EXPECT_EQ(req.version_minor, 1);
    EXPECT_EQ(req.body, "{\"a\":1}");
    ASSERT_TRUE(req.header("host").has_value());
    EXPECT_EQ(*req.header("host"), "127.0.0.1:8080");
    EXPECT_TRUE(req.keepAlive());
}

TEST(RequestParserTest, ByteByByteEquivalence) {
    std::string raw =
        "POST /v1/messages HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 11\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello world";
    RequestParser whole_parser;
    Buffer whole_buf;
    ASSERT_EQ(feedAll(whole_parser, whole_buf, raw), RequestParser::Result::complete);
    auto whole = whole_parser.take();

    RequestParser byte_parser;
    Buffer byte_buf;
    RequestParser::Result result = RequestParser::Result::need_more;
    for (char c : raw) {
        byte_buf.append(&c, 1);
        result = byte_parser.feed(byte_buf);
        if (result == RequestParser::Result::complete) break;
    }
    ASSERT_EQ(result, RequestParser::Result::complete);
    auto bytewise = byte_parser.take();

    EXPECT_EQ(whole.method, bytewise.method);
    EXPECT_EQ(whole.target, bytewise.target);
    EXPECT_EQ(whole.body, bytewise.body);
    EXPECT_EQ(whole.headers.size(), bytewise.headers.size());
    EXPECT_FALSE(bytewise.keepAlive());
}

TEST(RequestParserTest, Pipelining) {
    RequestParser parser;
    Buffer buf;
    std::string raw =
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    buf.append(raw);
    ASSERT_EQ(parser.feed(buf), RequestParser::Result::complete);
    auto first = parser.take();
    EXPECT_EQ(first.path, "/a");
    ASSERT_EQ(parser.feed(buf), RequestParser::Result::complete);
    auto second = parser.take();
    EXPECT_EQ(second.path, "/b");
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(RequestParserTest, HeaderTooLarge431) {
    RequestParser parser(ParserLimits{1024, 1024});
    Buffer buf;
    std::string raw = "GET / HTTP/1.1\r\nX-Big: " + std::string(2000, 'a') + "\r\n\r\n";
    EXPECT_EQ(feedAll(parser, buf, raw), RequestParser::Result::error);
    EXPECT_EQ(parser.errorStatus(), 431);
}

TEST(RequestParserTest, BodyTooLarge413) {
    RequestParser parser(ParserLimits{1024, 10});
    Buffer buf;
    std::string raw = "POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\n";
    EXPECT_EQ(feedAll(parser, buf, raw), RequestParser::Result::error);
    EXPECT_EQ(parser.errorStatus(), 413);
}

TEST(RequestParserTest, PostWithoutLength411) {
    RequestParser parser;
    Buffer buf;
    EXPECT_EQ(feedAll(parser, buf, "POST / HTTP/1.1\r\nHost: x\r\n\r\n"), RequestParser::Result::error);
    EXPECT_EQ(parser.errorStatus(), 411);
}

TEST(RequestParserTest, ChunkedRequest411) {
    RequestParser parser;
    Buffer buf;
    EXPECT_EQ(feedAll(parser, buf, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"),
              RequestParser::Result::error);
    EXPECT_EQ(parser.errorStatus(), 411);
}

TEST(RequestParserTest, BadRequest400) {
    {
        RequestParser parser;
        Buffer buf;
        EXPECT_EQ(feedAll(parser, buf, "GET /\r\n\r\n"), RequestParser::Result::error);
        EXPECT_EQ(parser.errorStatus(), 400);
    }
    {
        RequestParser parser;
        Buffer buf;
        EXPECT_EQ(feedAll(parser, buf, "GET / HTTP/2.0\r\n\r\n"), RequestParser::Result::error);
        EXPECT_EQ(parser.errorStatus(), 400);
    }
    {
        RequestParser parser;
        Buffer buf;
        EXPECT_EQ(feedAll(parser, buf, "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"),
                  RequestParser::Result::error);
        EXPECT_EQ(parser.errorStatus(), 400);
    }
    {
        RequestParser parser;
        Buffer buf;
        EXPECT_EQ(feedAll(parser, buf, "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n"), RequestParser::Result::error);
        EXPECT_EQ(parser.errorStatus(), 400);
    }
}

TEST(RequestParserTest, ExpectContinue) {
    RequestParser parser;
    Buffer buf;
    std::string head = "POST / HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n";
    EXPECT_EQ(feedAll(parser, buf, head), RequestParser::Result::need_more);
    EXPECT_TRUE(parser.expectContinue());
    parser.setContinueSent();
    EXPECT_FALSE(parser.expectContinue());
    buf.append("hello");
    EXPECT_EQ(parser.feed(buf), RequestParser::Result::complete);
    EXPECT_EQ(parser.take().body, "hello");
}

TEST(RequestParserTest, Http10KeepAlive) {
    {
        RequestParser parser;
        Buffer buf;
        ASSERT_EQ(feedAll(parser, buf, "GET / HTTP/1.0\r\n\r\n"), RequestParser::Result::complete);
        EXPECT_FALSE(parser.take().keepAlive());
    }
    {
        RequestParser parser;
        Buffer buf;
        ASSERT_EQ(feedAll(parser, buf, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
                  RequestParser::Result::complete);
        EXPECT_TRUE(parser.take().keepAlive());
    }
    {
        RequestParser parser;
        Buffer buf;
        ASSERT_EQ(feedAll(parser, buf, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"), RequestParser::Result::complete);
        EXPECT_FALSE(parser.take().keepAlive());
    }
    {
        RequestParser parser;
        Buffer buf;
        ASSERT_EQ(feedAll(parser, buf, "GET / HTTP/1.1\r\n\r\n"), RequestParser::Result::complete);
        EXPECT_TRUE(parser.take().keepAlive());
    }
}
