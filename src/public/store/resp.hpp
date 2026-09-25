#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gw::store {

// RESP2 回复值：simple / error / integer / bulk（可空）/ array
class RespValue {
public:
    enum class Type { simple, error, integer, bulk, array };

    RespValue() : type_(Type::bulk), null_(true) {}
    static RespValue simple(std::string s);
    static RespValue error(std::string s);
    static RespValue integer(std::int64_t v);
    static RespValue bulk(std::optional<std::string> s);
    static RespValue array(std::vector<RespValue> v);

    Type type() const { return type_; }
    bool isSimple() const { return type_ == Type::simple; }
    bool isError() const { return type_ == Type::error; }
    bool isInteger() const { return type_ == Type::integer; }
    bool isBulk() const { return type_ == Type::bulk; }
    bool isArray() const { return type_ == Type::array; }
    bool isNull() const { return null_; }

    // simple / error / bulk 的文本；bulk NULL 返回空串
    const std::string& str() const { return text_; }
    std::int64_t integer() const { return integer_; }
    const std::vector<RespValue>& items() const { return array_; }

private:
    Type type_ = Type::bulk;
    bool null_ = false;
    std::string text_;
    std::int64_t integer_ = 0;
    std::vector<RespValue> array_;
};

// RESP2 增量解码：数据可以在任意字节处切开，逐字节喂入与整段喂入结果一致
class RespParser {
public:
    enum class Result { need_more, ok, error };

    // 从 data 前缀解析一个完整回复；ok 时 consumed = 消耗的字节数、value 有效
    Result parse(std::string_view data, std::size_t& consumed, RespValue& value, std::string& error);

    void reset() { depth_ = 0; }

private:
    Result parseValue(std::string_view data, std::size_t& pos, RespValue& value, std::string& error);
    Result parseLine(std::string_view data, std::size_t& pos, std::string_view& line, std::string& error) const;

    int depth_ = 0;
    static constexpr int kMaxDepth = 64;
};

// 编码一条命令：*<n>\r\n$<len>\r\n<arg>\r\n…
std::string encodeCommand(const std::vector<std::string>& args);

// 便捷判断
inline const std::string* asError(const RespValue& v) { return v.isError() ? &v.str() : nullptr; }
inline const std::vector<RespValue>* asArray(const RespValue& v) { return v.isArray() ? &v.items() : nullptr; }
// bulk / integer → 整数（GET 返回 bulk、Lua 返回 integer）
std::optional<std::int64_t> valueToInt(const RespValue& v);

}  // namespace gw::store
