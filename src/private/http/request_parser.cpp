#include "http/request_parser.hpp"

#include <charconv>

#include "base/strings.hpp"

namespace gw::http {

namespace {

bool parseContentLength(std::string_view value, std::uint64_t* out) {
    if (value.empty()) return false;
    std::uint64_t result = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc{} || ptr != value.data() + value.size()) return false;
    *out = result;
    return true;
}

}  // namespace

RequestParser::RequestParser(ParserLimits limits) : limits_(limits) {}

void RequestParser::reset() {
    state_ = State::kRequestLine;
    request_ = HttpRequest{};
    header_bytes_ = 0;
    content_length_.reset();
    body_received_ = 0;
    chunked_ = false;
    expect_continue_ = false;
    continue_sent_ = false;
    headers_complete_ = false;
    error_status_ = 400;
}

bool RequestParser::parseRequestLine(std::string_view line) {
    auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;
    if (line.find(' ', sp2 + 1) != std::string_view::npos) return false;

    std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = line.substr(sp2 + 1);
    if (method.empty() || target.empty()) return false;

    if (version == "HTTP/1.1") {
        request_.version_minor = 1;
    } else if (version == "HTTP/1.0") {
        request_.version_minor = 0;
    } else {
        return false;
    }

    request_.method = std::string(method);
    request_.target = std::string(target);
    auto q = target.find('?');
    if (q == std::string_view::npos) {
        request_.path = std::string(target);
    } else {
        request_.path = std::string(target.substr(0, q));
        request_.query = std::string(target.substr(q + 1));
    }
    return true;
}

bool RequestParser::parseHeaderLine(std::string_view line) {
    auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c <= 32 || c == 127 || c == ':') return false;
        (void)uc;
    }
    std::string lower_name = toLower(trim(name));
    std::string trimmed_value = trim(value);

    if (lower_name == "content-length") {
        std::uint64_t len = 0;
        if (!parseContentLength(trimmed_value, &len)) return false;
        if (content_length_ && *content_length_ != len) return false;
        content_length_ = len;
    } else if (lower_name == "transfer-encoding") {
        if (iequals(trimmed_value, "chunked") || trimmed_value.find("chunked") != std::string::npos) {
            chunked_ = true;
        }
    } else if (lower_name == "expect") {
        if (iequals(trimmed_value, "100-continue")) expect_continue_ = true;
    }

    request_.headers.emplace_back(std::move(lower_name), std::move(trimmed_value));
    return true;
}

bool RequestParser::finishHeaders() {
    headers_complete_ = true;
    if (chunked_) {
        error_status_ = 411;
        return false;
    }
    if (!content_length_) {
        if (request_.method == "POST" || request_.method == "PUT" || request_.method == "PATCH") {
            error_status_ = 411;
            return false;
        }
        content_length_ = 0;
    }
    if (*content_length_ > limits_.max_body_bytes) {
        error_status_ = 413;
        return false;
    }
    return true;
}

RequestParser::Result RequestParser::feed(net::Buffer& in) {
    while (true) {
        if (state_ == State::kComplete) return Result::complete;
        if (state_ == State::kError) return Result::error;

        if (state_ == State::kRequestLine) {
            const char* crlf = in.findCRLF();
            if (crlf == nullptr) {
                if (header_bytes_ + in.readableBytes() > limits_.max_header_bytes) {
                    error_status_ = 431;
                    state_ = State::kError;
                    return Result::error;
                }
                return Result::need_more;
            }
            std::string_view line(in.peek(), static_cast<std::size_t>(crlf - in.peek()));
            header_bytes_ += line.size() + 2;
            if (header_bytes_ > limits_.max_header_bytes) {
                error_status_ = 431;
                state_ = State::kError;
                return Result::error;
            }
            if (!parseRequestLine(line)) {
                error_status_ = 400;
                state_ = State::kError;
                return Result::error;
            }
            in.retrieve(line.size() + 2);
            state_ = State::kHeaders;
        } else if (state_ == State::kHeaders) {
            const char* crlf = in.findCRLF();
            if (crlf == nullptr) {
                if (header_bytes_ + in.readableBytes() > limits_.max_header_bytes) {
                    error_status_ = 431;
                    state_ = State::kError;
                    return Result::error;
                }
                return Result::need_more;
            }
            std::string_view line(in.peek(), static_cast<std::size_t>(crlf - in.peek()));
            header_bytes_ += line.size() + 2;
            if (header_bytes_ > limits_.max_header_bytes) {
                error_status_ = 431;
                state_ = State::kError;
                return Result::error;
            }
            in.retrieve(line.size() + 2);
            if (line.empty()) {
                if (!finishHeaders()) {
                    state_ = State::kError;
                    return Result::error;
                }
                if (*content_length_ == 0) {
                    state_ = State::kComplete;
                    return Result::complete;
                }
                state_ = State::kBody;
            } else if (!parseHeaderLine(line)) {
                error_status_ = 400;
                state_ = State::kError;
                return Result::error;
            }
        } else if (state_ == State::kBody) {
            std::uint64_t remaining = *content_length_ - body_received_;
            if (in.readableBytes() < remaining) {
                body_received_ += in.readableBytes();
                request_.body.append(in.peek(), in.readableBytes());
                in.retrieveAll();
                return Result::need_more;
            }
            request_.body.append(in.peek(), static_cast<std::size_t>(remaining));
            in.retrieve(static_cast<std::size_t>(remaining));
            body_received_ = *content_length_;
            state_ = State::kComplete;
            return Result::complete;
        }
    }
}

HttpRequest RequestParser::take() {
    HttpRequest out = std::move(request_);
    reset();
    return out;
}

}  // namespace gw::http
