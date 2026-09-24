#include "http/chunked.hpp"

namespace gw::http {

namespace {

bool parseHex(std::string_view text, std::uint64_t* out) {
    if (text.empty() || text.size() > 16) return false;
    std::uint64_t value = 0;
    for (char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

}  // namespace

void ChunkedDecoder::reset() {
    state_ = State::kSizeLine;
    remaining_ = 0;
    size_line_len_ = 0;
    error_.clear();
}

ChunkedDecoder::Status ChunkedDecoder::feed(net::Buffer& in, std::string* out) {
    while (true) {
        switch (state_) {
            case State::kSizeLine: {
                const char* crlf = in.findCRLF();
                if (crlf == nullptr) {
                    size_line_len_ += in.readableBytes();
                    if (size_line_len_ > 256) {
                        error_ = "chunk size line too long";
                        state_ = State::kDone;
                        return Status::error;
                    }
                    return Status::need_more;
                }
                std::string_view line(in.peek(), static_cast<std::size_t>(crlf - in.peek()));
                auto semi = line.find(';');
                std::string_view size_text = semi == std::string_view::npos ? line : line.substr(0, semi);
                std::uint64_t size = 0;
                if (!parseHex(size_text, &size)) {
                    error_ = "invalid chunk size";
                    state_ = State::kDone;
                    return Status::error;
                }
                in.retrieve(line.size() + 2);
                size_line_len_ = 0;
                if (size == 0) {
                    state_ = State::kTrailer;
                } else {
                    remaining_ = size;
                    state_ = State::kData;
                }
                break;
            }
            case State::kData: {
                if (in.readableBytes() == 0) return Status::need_more;
                std::size_t take = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining_, static_cast<std::uint64_t>(in.readableBytes())));
                if (out != nullptr) out->append(in.peek(), take);
                in.retrieve(take);
                remaining_ -= take;
                if (remaining_ == 0) state_ = State::kDataCrlf;
                return Status::data;
            }
            case State::kDataCrlf: {
                if (in.readableBytes() < 2) return Status::need_more;
                const char* p = in.peek();
                if (p[0] != '\r' || p[1] != '\n') {
                    error_ = "missing CRLF after chunk data";
                    state_ = State::kDone;
                    return Status::error;
                }
                in.retrieve(2);
                state_ = State::kSizeLine;
                break;
            }
            case State::kTrailer: {
                const char* crlf = in.findCRLF();
                if (crlf == nullptr) return Status::need_more;
                std::size_t line_len = static_cast<std::size_t>(crlf - in.peek());
                in.retrieve(line_len + 2);
                if (line_len == 0) {
                    state_ = State::kDone;
                    return Status::done;
                }
                break;
            }
            case State::kDone:
                return Status::done;
        }
    }
}

}  // namespace gw::http
