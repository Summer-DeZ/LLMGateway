#include "gateway/token_bucket.hpp"

#include <algorithm>

namespace gw::gateway {

TokenBucket::TokenBucket(const Clock& clock, double capacity, double refill_per_sec)
    : clock_(&clock), capacity_(capacity), refill_per_sec_(refill_per_sec), tokens_(capacity), last_(clock.now()) {
    if (capacity_ < 0) capacity_ = 0;
    if (refill_per_sec_ < 0) refill_per_sec_ = 0;
    if (tokens_ > capacity_) tokens_ = capacity_;
}

void TokenBucket::refill(Timestamp now) {
    if (now > last_) {
        double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_).count();
        tokens_ = std::min(capacity_, tokens_ + seconds * refill_per_sec_);
        last_ = now;
    }
}

bool TokenBucket::canTake(double n) const {
    if (n > capacity_) return tokens_ >= capacity_;
    return tokens_ >= n;
}

void TokenBucket::take(double n) {
    tokens_ -= n;
}

void TokenBucket::adjust(double delta) {
    tokens_ = std::clamp(tokens_ + delta, -capacity_, capacity_);
}

Duration TokenBucket::waitFor(double n) const {
    double target = n > capacity_ ? capacity_ : n;
    if (tokens_ >= target) return Duration::zero();
    if (refill_per_sec_ <= 0) {
        // 速率不为 0 的桶才可能等到；速率为 0 且不足时给一个很长的等待（实际不会出现）
        return std::chrono::hours(1);
    }
    double seconds = (target - tokens_) / refill_per_sec_;
    return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(seconds));
}

}  // namespace gw::gateway
