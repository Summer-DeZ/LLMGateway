#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "http/http_request.hpp"
#include "net/buffer.hpp"

namespace gw::http {

struct ParserLimits {
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
};

class RequestParser {
public:
    enum class Result { need_more, complete, error };
    enum class State { kRequestLine, kHeaders, kBody, kComplete, kError };

    explicit RequestParser(ParserLimits limits = {});

    // 消费已解析的字节；complete 时剩余字节留在 Buffer（pipelining）
    Result feed(net::Buffer& in);
    HttpRequest take();
    int errorStatus() const { return error_status_; }
    bool expectContinue() const { return expect_continue_ && !continue_sent_; }
    void setContinueSent() { continue_sent_ = true; }
    bool headersComplete() const { return headers_complete_; }
    void reset();
    State state() const { return state_; }

private:
    bool parseRequestLine(std::string_view line);
    bool parseHeaderLine(std::string_view line);
    bool finishHeaders();

    ParserLimits limits_;
    State state_ = State::kRequestLine;
    HttpRequest request_;
    std::size_t header_bytes_ = 0;
    std::optional<std::uint64_t> content_length_;
    std::uint64_t body_received_ = 0;
    bool chunked_ = false;
    bool expect_continue_ = false;
    bool continue_sent_ = false;
    bool headers_complete_ = false;
    int error_status_ = 400;
};

}  // namespace gw::http
