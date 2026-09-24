#include "http/response_parser.hpp"

#include <charconv>

#include "base/strings.hpp"

namespace gw::http {

namespace {

bool parseUint(std::string_view text, std::uint64_t* out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) return false;
    *out = value;
    return true;
}

bool parseStatusLine(std::string_view line, int* status) {
    if (!line.starts_with("HTTP/")) return false;
    auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    auto sp2 = line.find(' ', sp1 + 1);
    std::string_view code = line.substr(sp1 + 1, sp2 == std::string_view::npos ? std::string_view::npos : sp2 - sp1 - 1);
    unsigned value = 0;
    auto [ptr, ec] = std::from_chars(code.data(), code.data() + code.size(), value);
    if (ec != std::errc{} || ptr != code.data() + code.size() || value < 100 || value > 599) return false;
    *status = static_cast<int>(value);
    return true;
}

}  // namespace

ResponseParser::ResponseParser(ResponseParserLimits limits, Callbacks callbacks)
    : limits_(limits), callbacks_(std::move(callbacks)) {}

void ResponseParser::feed(net::Buffer& in) {
    while (state_ != State::kComplete && state_ != State::kError) {
        if (state_ == State::kHead) {
            const char* end = in.findDoubleCRLF();
            if (end == nullptr) {
                head_bytes_ += in.readableBytes();
                if (head_bytes_ > limits_.max_head_bytes) {
                    fail("response head too large");
                }
                return;
            }
            parseHead(in);
            if (state_ == State::kHead) continue;  // 1xx 已跳过，继续解析下一个响应头
            if (state_ != State::kBody) return;
            continue;
        }
        if (state_ == State::kBody) {
            if (framing_ == Framing::kContentLength) {
                std::uint64_t remaining = *content_length_ - body_received_;
                if (in.readableBytes() == 0) return;
                std::size_t take = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(in.readableBytes())));
                if (body_received_ + take > limits_.max_body_bytes) {
                    fail("response body too large");
                    return;
                }
                emitData(std::string_view(in.peek(), take));
                in.retrieve(take);
                body_received_ += take;
                if (body_received_ == *content_length_) {
                    complete(reusable_);
                }
                return;
            }
            if (framing_ == Framing::kChunked) {
                while (true) {
                    std::string chunk;
                    ChunkedDecoder::Status status = chunked_.feed(in, &chunk);
                    if (!chunk.empty()) {
                        if (body_received_ + chunk.size() > limits_.max_body_bytes) {
                            fail("response body too large");
                            return;
                        }
                        body_received_ += chunk.size();
                        emitData(chunk);
                    }
                    if (status == ChunkedDecoder::Status::error) {
                        fail("chunked decode error: " + chunked_.error());
                        return;
                    }
                    if (status == ChunkedDecoder::Status::done) {
                        complete(reusable_);
                        return;
                    }
                    if (status == ChunkedDecoder::Status::need_more) return;
                }
            }
            // kUntilClose
            if (in.readableBytes() == 0) return;
            std::size_t take = in.readableBytes();
            if (body_received_ + take > limits_.max_body_bytes) {
                fail("response body too large");
                return;
            }
            body_received_ += take;
            emitData(std::string_view(in.peek(), take));
            in.retrieveAll();
            return;
        }
    }
}

void ResponseParser::parseHead(net::Buffer& in) {
    const char* end = in.findDoubleCRLF();
    std::size_t head_len = static_cast<std::size_t>(end - in.peek()) + 4;
    std::string_view head(in.peek(), head_len);
    in.retrieve(head_len);

    std::size_t pos = 0;
    auto nextLine = [&](std::string_view* line) -> bool {
        auto crlf = head.find("\r\n", pos);
        if (crlf == std::string_view::npos) return false;
        *line = head.substr(pos, crlf - pos);
        pos = crlf + 2;
        return true;
    };

    std::string_view status_line;
    if (!nextLine(&status_line)) {
        fail("missing status line");
        return;
    }
    int status = 0;
    if (!parseStatusLine(status_line, &status)) {
        fail("invalid status line");
        return;
    }

    Headers headers;
    std::string_view line;
    while (nextLine(&line)) {
        if (line.empty()) continue;
        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            fail("invalid response header");
            return;
        }
        headers.emplace_back(toLower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }

    if (status >= 100 && status < 200) {
        // 跳过 1xx，继续解析下一个响应
        head_bytes_ = 0;
        return;
    }

    status_ = status;
    headers_ = headers;
    head_received_ = true;

    bool has_chunked = false;
    bool connection_close = false;
    std::optional<std::uint64_t> content_length;
    for (const auto& [name, value] : headers_) {
        if (name == "transfer-encoding" && value.find("chunked") != std::string::npos) has_chunked = true;
        if (name == "content-length") {
            std::uint64_t len = 0;
            if (!parseUint(trim(value), &len)) {
                fail("invalid content-length");
                return;
            }
            if (content_length && *content_length != len) {
                fail("conflicting content-length");
                return;
            }
            content_length = len;
        }
        if (name == "connection" && HttpRequest::containsToken(value, "close")) connection_close = true;
    }

    if (has_chunked) {
        framing_ = Framing::kChunked;
    } else if (content_length) {
        framing_ = Framing::kContentLength;
        content_length_ = content_length;
    } else {
        framing_ = Framing::kUntilClose;
        reusable_ = false;
    }
    if (connection_close) reusable_ = false;
    if (status_ >= 400) reusable_ = false;

    if (callbacks_.on_head) callbacks_.on_head(status_, headers_);

    if (framing_ == Framing::kContentLength && *content_length_ == 0) {
        complete(reusable_);
        return;
    }
    state_ = State::kBody;
}

void ResponseParser::emitData(std::string_view data) {
    if (status_ >= 400) {
        if (error_body_emitted_ >= limits_.max_error_body_bytes) {
            body_truncated_ = true;
            reusable_ = false;
            return;
        }
        std::size_t take = std::min(data.size(), limits_.max_error_body_bytes - error_body_emitted_);
        if (take < data.size()) {
            body_truncated_ = true;
            reusable_ = false;
        }
        error_body_emitted_ += take;
        if (callbacks_.on_data) callbacks_.on_data(data.substr(0, take));
        return;
    }
    if (callbacks_.on_data) callbacks_.on_data(data);
}

void ResponseParser::complete(bool reusable) {
    state_ = State::kComplete;
    reusable_ = reusable;
    if (callbacks_.on_complete) callbacks_.on_complete(reusable_);
}

void ResponseParser::fail(std::string message) {
    state_ = State::kError;
    error_ = std::move(message);
    reusable_ = false;
    if (callbacks_.on_error) callbacks_.on_error(error_);
}

void ResponseParser::feedEof() {
    if (state_ == State::kComplete || state_ == State::kError) return;
    if (!head_received_) {
        fail("connection closed before response head");
        return;
    }
    if (framing_ == Framing::kUntilClose && state_ == State::kBody) {
        complete(false);
        return;
    }
    fail("connection closed before response complete");
}

}  // namespace gw::http
