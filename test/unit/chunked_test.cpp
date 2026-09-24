#include "http/chunked.hpp"

#include <gtest/gtest.h>

#include <string>

#include "net/buffer.hpp"

using gw::http::ChunkedDecoder;
using gw::net::Buffer;

TEST(ChunkedTest, SimpleBody) {
    Buffer buf;
    buf.append(std::string_view("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"));
    ChunkedDecoder decoder;
    std::string out;
    EXPECT_EQ(decoder.feed(buf, &out), ChunkedDecoder::Status::data);
    EXPECT_EQ(decoder.feed(buf, &out), ChunkedDecoder::Status::data);
    EXPECT_EQ(decoder.feed(buf, &out), ChunkedDecoder::Status::done);
    EXPECT_EQ(out, "Wikipedia");
}

TEST(ChunkedTest, ExtensionsAndTrailers) {
    Buffer buf;
    buf.append("4;ext=1\r\nWiki\r\n0\r\nX-Trailer: 1\r\n\r\n");
    ChunkedDecoder decoder;
    std::string out;
    ChunkedDecoder::Status status;
    do {
        status = decoder.feed(buf, &out);
    } while (status == ChunkedDecoder::Status::data);
    EXPECT_EQ(status, ChunkedDecoder::Status::done);
    EXPECT_EQ(out, "Wiki");
}

TEST(ChunkedTest, SplitAcrossChunks) {
    std::string raw = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    ChunkedDecoder decoder;
    Buffer buf;
    std::string out;
    for (char c : raw) {
        buf.append(&c, 1);
        ChunkedDecoder::Status status = decoder.feed(buf, &out);
        if (status == ChunkedDecoder::Status::done) break;
    }
    EXPECT_TRUE(decoder.done());
    EXPECT_EQ(out, "Wikipedia");
}

TEST(ChunkedTest, InvalidHex) {
    Buffer buf;
    buf.append("zz\r\n", 4);
    ChunkedDecoder decoder;
    std::string out;
    EXPECT_EQ(decoder.feed(buf, &out), ChunkedDecoder::Status::error);
    EXPECT_FALSE(decoder.error().empty());
}

TEST(ChunkedTest, TooLongSizeLine) {
    Buffer buf;
    buf.append(std::string(300, '1'));
    ChunkedDecoder decoder;
    std::string out;
    EXPECT_EQ(decoder.feed(buf, &out), ChunkedDecoder::Status::error);
}

TEST(ChunkedTest, MissingDataCrlf) {
    Buffer buf;
    buf.append("4\r\nWikiXX0\r\n\r\n");
    ChunkedDecoder decoder;
    std::string out;
    EXPECT_EQ(decoder.feed(buf, &out), ChunkedDecoder::Status::data);
    EXPECT_EQ(decoder.feed(buf, &out), ChunkedDecoder::Status::error);
}
