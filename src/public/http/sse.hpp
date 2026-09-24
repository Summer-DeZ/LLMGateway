#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace gw::http {

struct SseEvent {
    std::string event;
    std::string data;
    std::string id;
};

// 与 DAgent net::SseParser 语义一致：LF/CRLF/单独 CR、BOM、注释、多行 data。
class SseParser {
public:
    using EventCallback = std::function<void(SseEvent&&)>;

    void feed(std::string_view data, const EventCallback& on_event);
    // 流结束时未以空行结尾的事件丢弃
    void reset();

    std::size_t bytesSinceLastEvent() const { return bytes_since_last_event_; }

private:
    void processLine(std::string_view line, const EventCallback& on_event);
    void dispatch(const EventCallback& on_event);
    void handleChar(char c, const EventCallback& on_event);

    std::string line_buffer_;
    SseEvent current_;
    bool has_data_ = false;
    std::size_t bytes_since_last_event_ = 0;
    bool first_line_ = true;
    bool pending_cr_ = false;
    bool saw_bom_ = false;
};

}  // namespace gw::http
