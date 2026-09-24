#include "http/sse.hpp"

namespace gw::http {

void SseParser::reset() {
    line_buffer_.clear();
    current_ = SseEvent{};
    has_data_ = false;
    bytes_since_last_event_ = 0;
    first_line_ = true;
    pending_cr_ = false;
    saw_bom_ = false;
}

void SseParser::dispatch(const EventCallback& on_event) {
    // 与 DAgent 一致：见过 data 字段才派发（id 按规范跨事件保留）
    if (has_data_) {
        std::string id = current_.id;
        if (on_event) on_event(std::move(current_));
        current_ = SseEvent{};
        current_.id = std::move(id);
        has_data_ = false;
        bytes_since_last_event_ = 0;
    } else {
        current_.event.clear();
        current_.data.clear();
    }
}

void SseParser::processLine(std::string_view line, const EventCallback& on_event) {
    if (first_line_) {
        first_line_ = false;
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
            line.remove_prefix(3);
        }
    }
    if (line.empty()) {
        dispatch(on_event);
        return;
    }
    if (line.front() == ':') {
        return;  // 注释
    }
    std::size_t colon = line.find(':');
    std::string_view field = colon == std::string_view::npos ? line : line.substr(0, colon);
    std::string_view value;
    if (colon != std::string_view::npos) {
        value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    }
    if (field == "data") {
        if (has_data_) current_.data.push_back('\n');
        current_.data.append(value);
        has_data_ = true;
    } else if (field == "event") {
        current_.event.assign(value);
    } else if (field == "id") {
        current_.id.assign(value);
    }
    // retry 忽略
}

void SseParser::handleChar(char c, const EventCallback& on_event) {
    if (c == '\r') {
        processLine(line_buffer_, on_event);
        line_buffer_.clear();
        pending_cr_ = true;
        return;
    }
    if (c == '\n') {
        if (pending_cr_) {
            pending_cr_ = false;  // CRLF 已经处理过 CR
            return;
        }
        processLine(line_buffer_, on_event);
        line_buffer_.clear();
        return;
    }
    if (pending_cr_) {
        pending_cr_ = false;  // 单独 CR 作为行尾
    }
    line_buffer_.push_back(c);
}

void SseParser::feed(std::string_view data, const EventCallback& on_event) {
    bytes_since_last_event_ += data.size();
    for (char c : data) {
        handleChar(c, on_event);
    }
}

}  // namespace gw::http
