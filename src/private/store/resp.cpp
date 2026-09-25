#include "store/resp.hpp"

#include <charconv>
#include <utility>

namespace gw::store {

namespace {
constexpr std::string_view kCrlf = "\r\n";
}

RespValue RespValue::simple(std::string s) {
    RespValue v;
    v.type_ = Type::simple;
    v.text_ = std::move(s);
    return v;
}

RespValue RespValue::error(std::string s) {
    RespValue v;
    v.type_ = Type::error;
    v.text_ = std::move(s);
    return v;
}

RespValue RespValue::integer(std::int64_t n) {
    RespValue v;
    v.type_ = Type::integer;
    v.integer_ = n;
    return v;
}

RespValue RespValue::bulk(std::optional<std::string> s) {
    RespValue v;
    v.type_ = Type::bulk;
    v.null_ = !s.has_value();
    if (s) v.text_ = std::move(*s);
    return v;
}

RespValue RespValue::array(std::vector<RespValue> v) {
    RespValue out;
    out.type_ = Type::array;
    out.array_ = std::move(v);
    return out;
}

std::optional<std::int64_t> valueToInt(const RespValue& v) {
    if (v.isInteger()) return v.integer();
    if (v.isBulk() && !v.isNull()) {
        const std::string& s = v.str();
        std::int64_t out = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        if (ec == std::errc{} && ptr == s.data() + s.size()) return out;
    }
    return std::nullopt;
}

std::string encodeCommand(const std::vector<std::string>& args) {
    std::string out;
    std::size_t total = 0;
    for (const auto& a : args) total += a.size();
    out.reserve(total + args.size() * 16 + 16);
    out.push_back('*');
    out.append(std::to_string(args.size()));
    out.append(kCrlf);
    for (const auto& a : args) {
        out.push_back('$');
        out.append(std::to_string(a.size()));
        out.append(kCrlf);
        out.append(a);
        out.append(kCrlf);
    }
    return out;
}

RespParser::Result RespParser::parseLine(std::string_view data, std::size_t& pos, std::string_view& line,
                                         std::string& error) const {
    std::size_t eol = data.find("\r\n", pos);
    if (eol == std::string_view::npos) return Result::need_more;
    line = data.substr(pos, eol - pos);
    pos = eol + 2;
    (void)error;
    return Result::ok;
}

RespParser::Result RespParser::parse(std::string_view data, std::size_t& consumed, RespValue& value,
                                     std::string& error) {
    depth_ = 0;
    std::size_t pos = 0;
    Result r = parseValue(data, pos, value, error);
    if (r == Result::ok) consumed = pos;
    return r;
}

RespParser::Result RespParser::parseValue(std::string_view data, std::size_t& pos, RespValue& value,
                                          std::string& error) {
    if (pos >= data.size()) return Result::need_more;
    char type = data[pos];
    std::string_view line;
    switch (type) {
        case '+': {
            if (parseLine(data, pos, line, error) != Result::ok) return Result::need_more;
            line.remove_prefix(1);
            value = RespValue::simple(std::string(line));
            return Result::ok;
        }
        case '-': {
            if (parseLine(data, pos, line, error) != Result::ok) return Result::need_more;
            line.remove_prefix(1);
            value = RespValue::error(std::string(line));
            return Result::ok;
        }
        case ':': {
            if (parseLine(data, pos, line, error) != Result::ok) return Result::need_more;
            std::int64_t n = 0;
            auto [ptr, ec] = std::from_chars(line.data() + 1, line.data() + line.size(), n);
            if (ec != std::errc{} || ptr != line.data() + line.size()) {
                error = "bad integer reply";
                return Result::error;
            }
            value = RespValue::integer(n);
            return Result::ok;
        }
        case '$': {
            std::size_t header_pos = pos;
            if (parseLine(data, header_pos, line, error) != Result::ok) return Result::need_more;
            std::int64_t len = 0;
            auto [ptr, ec] = std::from_chars(line.data() + 1, line.data() + line.size(), len);
            if (ec != std::errc{} || ptr != line.data() + line.size() || len < -1) {
                error = "bad bulk length";
                return Result::error;
            }
            if (len == -1) {
                pos = header_pos;
                value = RespValue::bulk(std::nullopt);
                return Result::ok;
            }
            std::size_t body = header_pos + static_cast<std::size_t>(len);
            if (data.size() < body + 2) return Result::need_more;
            if (data[body] != '\r' || data[body + 1] != '\n') {
                error = "bulk missing CRLF";
                return Result::error;
            }
            value = RespValue::bulk(std::string(data.substr(header_pos, static_cast<std::size_t>(len))));
            pos = body + 2;
            return Result::ok;
        }
        case '*': {
            std::size_t header_pos = pos;
            if (parseLine(data, header_pos, line, error) != Result::ok) return Result::need_more;
            std::int64_t count = 0;
            auto [ptr, ec] = std::from_chars(line.data() + 1, line.data() + line.size(), count);
            if (ec != std::errc{} || ptr != line.data() + line.size() || count < -1) {
                error = "bad array length";
                return Result::error;
            }
            pos = header_pos;
            if (count == -1) {
                value = RespValue::array({});  // NULL 数组按空数组处理
                return Result::ok;
            }
            if (depth_ + 1 > kMaxDepth) {
                error = "array nesting too deep";
                return Result::error;
            }
            std::vector<RespValue> arr;
            arr.reserve(static_cast<std::size_t>(count));
            ++depth_;
            for (std::int64_t i = 0; i < count; ++i) {
                RespValue element;
                Result r = parseValue(data, pos, element, error);
                if (r != Result::ok) {
                    --depth_;
                    return r;  // need_more / error 都直接向上传（调用方未消费数据，会整体重解析）
                }
                arr.push_back(std::move(element));
            }
            --depth_;
            value = RespValue::array(std::move(arr));
            return Result::ok;
        }
        default:
            error = "unknown reply type";
            return Result::error;
    }
}

}  // namespace gw::store
