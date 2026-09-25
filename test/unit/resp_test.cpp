#include <gtest/gtest.h>

#include <string>

#include "store/resp.hpp"

namespace {

using namespace std::string_literals;
using gw::store::RespParser;
using gw::store::RespValue;

// 逐字节喂入与整段喂入结果一致（所有解析器类单测的通用要求）
template <typename Fn>
void feedByteByByte(const std::string& data, Fn&& checker) {
    RespParser p;
    std::string buf;
    for (char c : data) {
        buf.push_back(c);
    }
    // 整段
    {
        RespParser parser;
        RespValue v;
        std::string err;
        std::size_t consumed = 0;
        ASSERT_EQ(parser.parse(buf, consumed, v, err), RespParser::Result::ok) << err;
        checker(v);
        ASSERT_EQ(consumed, buf.size());
    }
    // 逐字节（need_more 之后最终 ok）
    {
        RespParser parser;
        RespValue v;
        std::string err;
        std::size_t consumed = 0;
        for (std::size_t i = 1; i < buf.size(); ++i) {
            ASSERT_EQ(parser.parse(std::string_view(buf).substr(0, i), consumed, v, err),
                      RespParser::Result::need_more)
                << "i=" << i;
        }
        ASSERT_EQ(parser.parse(buf, consumed, v, err), RespParser::Result::ok) << err;
        checker(v);
    }
}

TEST(RespTest, SimpleString) {
    feedByteByByte("+OK\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isSimple());
        EXPECT_EQ(v.str(), "OK");
    });
}

TEST(RespTest, ErrorReply) {
    feedByteByByte("-ERR unknown command\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isError());
        EXPECT_EQ(v.str(), "ERR unknown command");
    });
}

TEST(RespTest, Integer) {
    feedByteByByte(":1024\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isInteger());
        EXPECT_EQ(v.integer(), 1024);
    });
    // 负数
    feedByteByByte(":-7\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isInteger());
        EXPECT_EQ(v.integer(), -7);
    });
}

TEST(RespTest, BulkString) {
    feedByteByByte("$5\r\nhello\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isBulk());
        ASSERT_FALSE(v.isNull());
        EXPECT_EQ(v.str(), "hello");
    });
}

TEST(RespTest, NullBulk) {
    feedByteByByte("$-1\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isBulk());
        EXPECT_TRUE(v.isNull());
    });
}

TEST(RespTest, EmptyBulk) {
    feedByteByByte("$0\r\n\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isBulk());
        ASSERT_FALSE(v.isNull());
        EXPECT_EQ(v.str(), "");
    });
}

TEST(RespTest, NestedArray) {
    feedByteByByte("*3\r\n:1\r\n$5\r\nhello\r\n*2\r\n+OK\r\n$-1\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isArray());
        const auto& items = v.items();
        ASSERT_EQ(items.size(), 3u);
        EXPECT_EQ(items[0].integer(), 1);
        EXPECT_EQ(items[1].str(), "hello");
        ASSERT_TRUE(items[2].isArray());
        EXPECT_EQ(items[2].items().size(), 2u);
        EXPECT_TRUE(items[2].items()[0].isSimple());
        EXPECT_TRUE(items[2].items()[1].isNull());
    });
}

TEST(RespTest, EmptyArray) {
    feedByteByByte("*0\r\n", [](const RespValue& v) {
        ASSERT_TRUE(v.isArray());
        EXPECT_EQ(v.items().size(), 0u);
    });
}

TEST(RespTest, PipelinedReplies) {
    // 多个回复连在一起：逐个解析并正确推进 consumed
    RespParser parser;
    std::string data = "+OK\r\n:42\r\n$3\r\nfoo\r\n";
    std::size_t pos = 0;
    RespValue v;
    std::string err;
    std::size_t consumed = 0;
    ASSERT_EQ(parser.parse(std::string_view(data).substr(pos), consumed, v, err), RespParser::Result::ok);
    EXPECT_TRUE(v.isSimple());
    pos += consumed;
    ASSERT_EQ(parser.parse(std::string_view(data).substr(pos), consumed, v, err), RespParser::Result::ok);
    EXPECT_EQ(v.integer(), 42);
    pos += consumed;
    ASSERT_EQ(parser.parse(std::string_view(data).substr(pos), consumed, v, err), RespParser::Result::ok);
    EXPECT_EQ(v.str(), "foo");
    pos += consumed;
    EXPECT_EQ(pos, data.size());
}

TEST(RespTest, RejectsGarbage) {
    RespParser parser;
    RespValue v;
    std::string err;
    std::size_t consumed = 0;
    EXPECT_EQ(parser.parse("x\r\n", consumed, v, err), RespParser::Result::error);
    EXPECT_EQ(parser.parse("*x\r\n", consumed, v, err), RespParser::Result::error);
    EXPECT_EQ(parser.parse("$abc\r\n", consumed, v, err), RespParser::Result::error);
}

TEST(RespTest, EncodeCommandAndRoundTrip) {
    auto encoded = gw::store::encodeCommand({"EVALSHA", "abc123", "2", "k1", "k2"});
    EXPECT_EQ(encoded, "*5\r\n$7\r\nEVALSHA\r\n$6\r\nabc123\r\n$1\r\n2\r\n$2\r\nk1\r\n$2\r\nk2\r\n");
    // 网关侧把命令喂给服务器时，RESP 解码应还原参数个数（此处只验证编码合法：长度字段与内容一致）
    RespParser parser;
    RespValue v;
    std::string err;
    std::size_t consumed = 0;
    // 服务端不会回复请求格式，但 RESP 解析器对请求编码同样是合法的 RESP 数组
    ASSERT_EQ(parser.parse(encoded, consumed, v, err), RespParser::Result::ok);
    ASSERT_TRUE(v.isArray());
    EXPECT_EQ(v.items().size(), 5u);
}

TEST(RespTest, ValueToIntHelpers) {
    EXPECT_EQ(gw::store::valueToInt(RespValue::bulk("12345")), 12345);
    EXPECT_EQ(gw::store::valueToInt(RespValue::integer(-5)), -5);
    EXPECT_FALSE(gw::store::valueToInt(RespValue::bulk("abc")).has_value());
    EXPECT_FALSE(gw::store::valueToInt(RespValue::bulk(std::nullopt)).has_value());
}

}  // namespace
