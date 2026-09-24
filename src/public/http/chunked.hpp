#pragma once

#include <cstdint>
#include <string>

#include "net/buffer.hpp"

namespace gw::http {

// chunked 传输编码的增量解码器（不含 HTTP 头）。
class ChunkedDecoder {
public:
    enum class Status { need_more, data, done, error };

    Status feed(net::Buffer& in, std::string* out);
    void reset();
    const std::string& error() const { return error_; }
    bool done() const { return state_ == State::kDone; }

private:
    enum class State { kSizeLine, kData, kDataCrlf, kTrailer, kDone };

    State state_ = State::kSizeLine;
    std::uint64_t remaining_ = 0;
    std::size_t size_line_len_ = 0;
    std::string error_;
};

}  // namespace gw::http
