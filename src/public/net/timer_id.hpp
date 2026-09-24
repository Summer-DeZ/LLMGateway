#pragma once

#include <cstdint>

namespace gw::net {

struct Timer;
class TimerQueue;

// 定时器句柄；sequence 防止 Timer 地址复用导致误取消。
class TimerId {
public:
    TimerId() = default;
    TimerId(Timer* timer, std::int64_t seq) : timer_(timer), sequence_(seq) {}

    bool valid() const { return timer_ != nullptr; }

    friend class TimerQueue;

private:
    Timer* timer_ = nullptr;
    std::int64_t sequence_ = 0;
};

}  // namespace gw::net
