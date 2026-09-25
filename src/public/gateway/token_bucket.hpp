#pragma once

#include <chrono>
#include <mutex>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"

namespace gw::gateway {

// 可注入的时钟（单测用假时钟推进时间）
class Clock {
public:
    virtual ~Clock() = default;
    virtual Timestamp now() const = 0;
};

class SteadyClock final : public Clock {
public:
    Timestamp now() const override { return std::chrono::steady_clock::now(); }
};

// 每桶一把 mutex；下面的操作需在持锁状态下调用（RateLimiter 用 std::lock 统一加锁，policies.md §1.2）
class TokenBucket : noncopyable {
public:
    TokenBucket(const Clock& clock, double capacity, double refill_per_sec);

    void refill(Timestamp now);  // tokens = min(capacity, tokens + elapsed × rate)
    bool canTake(double n) const;  // n > capacity 时要求桶满（否则大请求永远无法通过）
    void take(double n);
    void adjust(double delta);  // 结算：可变负，下限 -capacity
    Duration waitFor(double n) const;  // 距离 canTake(n) 为真还需多久

    double tokens() const { return tokens_; }
    double capacity() const { return capacity_; }
    std::mutex& mutex() const { return mutex_; }

private:
    const Clock* clock_;
    double capacity_;
    double refill_per_sec_;
    double tokens_;
    Timestamp last_;
    mutable std::mutex mutex_;
};

}  // namespace gw::gateway
