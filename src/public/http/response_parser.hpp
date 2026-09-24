#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "http/chunked.hpp"
#include "http/http_request.hpp"
#include "net/buffer.hpp"

namespace gw::http {

struct ResponseParserLimits {
    std::size_t max_head_bytes = 64 * 1024;
    std::size_t max_error_body_bytes = 64 * 1024;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
};

// 上游响应解析：Content-Length / chunked / 连接关闭定界；1xx 跳过。
class ResponseParser {
public:
    struct Callbacks {
        std::function<void(int status, const Headers&)> on_head;
        std::function<void(std::string_view)> on_data;
        std::function<void(bool reusable)> on_complete;
        std::function<void(std::string)> on_error;
    };

    ResponseParser(ResponseParserLimits limits, Callbacks callbacks);

    void feed(net::Buffer& in);
    // 连接在响应未完整结束时关闭
    void feedEof();

    bool headReceived() const { return head_received_; }
    bool complete() const { return state_ == State::kComplete; }
    bool failed() const { return state_ == State::kError; }
    bool reusable() const { return reusable_; }
    int status() const { return status_; }
    const Headers& headers() const { return headers_; }
    const std::string& error() const { return error_; }

private:
    enum class State { kHead, kBody, kComplete, kError };
    enum class Framing { kContentLength, kChunked, kUntilClose };

    void parseHead(net::Buffer& in);
    void emitData(std::string_view data);
    void complete(bool reusable);
    void fail(std::string message);

    ResponseParserLimits limits_;
    Callbacks callbacks_;
    State state_ = State::kHead;
    Framing framing_ = Framing::kUntilClose;
    std::optional<std::uint64_t> content_length_;
    std::uint64_t body_received_ = 0;
    std::size_t error_body_emitted_ = 0;
    std::size_t head_bytes_ = 0;
    bool head_received_ = false;
    bool reusable_ = true;
    bool body_truncated_ = false;
    int status_ = 0;
    Headers headers_;
    ChunkedDecoder chunked_;
    std::string error_;
};

}  // namespace gw::http
