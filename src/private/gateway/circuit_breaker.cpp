#include "gateway/circuit_breaker.hpp"

#include <algorithm>

namespace gw::gateway {

namespace {
constexpr auto kMaxCooldown = std::chrono::milliseconds(5 * 60 * 1000);
}

CircuitBreaker::CircuitBreaker(const BreakerConfig& config, const Clock& clock)
    : config_(config), clock_(&clock), current_cooldown_(config.cooldown) {}

const char* CircuitBreaker::stateName(State state) {
    switch (state) {
        case State::closed: return "closed";
        case State::open: return "open";
        case State::half_open: return "half_open";
    }
    return "?";
}

CircuitBreaker::State CircuitBreaker::state() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

void CircuitBreaker::setStateCallback(std::function<void(State)> cb) {
    std::scoped_lock lock(mutex_);
    state_callback_ = std::move(cb);
}

bool CircuitBreaker::allowRequest() {
    std::scoped_lock lock(mutex_);
    Timestamp now = clock_->now();
    switch (state_) {
        case State::closed:
            return true;
        case State::open: {
            if (now - opened_at_ < current_cooldown_) return false;
            state_ = State::half_open;
            probe_outstanding_ = true;
            probe_started_at_ = now;
            if (state_callback_) state_callback_(state_);
            return true;
        }
        case State::half_open: {
            if (!probe_outstanding_) {
                probe_outstanding_ = true;
                probe_started_at_ = now;
                return true;
            }
            return false;
        }
    }
    return false;
}

void CircuitBreaker::recordSuccess() {
    std::scoped_lock lock(mutex_);
    consecutive_failures_ = 0;
    probe_outstanding_ = false;
    current_cooldown_ = config_.cooldown;
    if (state_ != State::closed) {
        state_ = State::closed;
        if (state_callback_) state_callback_(state_);
    }
}

void CircuitBreaker::recordFailure() {
    std::scoped_lock lock(mutex_);
    Timestamp now = clock_->now();
    switch (state_) {
        case State::closed:
            ++consecutive_failures_;
            if (config_.failure_threshold > 0 && consecutive_failures_ >= config_.failure_threshold) {
                toOpen(now, false);
            }
            break;
        case State::half_open:
            toOpen(now, true);  // 探测失败：冷却翻倍
            break;
        case State::open:
            break;
    }
}

void CircuitBreaker::toOpen(Timestamp now, bool double_cooldown) {
    state_ = State::open;
    opened_at_ = now;
    probe_outstanding_ = false;
    consecutive_failures_ = 0;
    if (double_cooldown) {
        current_cooldown_ = std::min(kMaxCooldown, current_cooldown_ * 2);
    }
    if (state_callback_) state_callback_(state_);
}

void CircuitBreaker::releaseProbe() {
    std::scoped_lock lock(mutex_);
    if (state_ == State::half_open) probe_outstanding_ = false;
}

// ===== CircuitBreakerSet =====

CircuitBreakerSet::CircuitBreakerSet(const Config& config, const Clock& clock) {
    for (const auto& [name, upstream] : config.upstreams) {
        (void)name;
        breakers_.emplace(upstream.name, std::make_unique<CircuitBreaker>(upstream.breaker, clock));
    }
}

CircuitBreaker* CircuitBreakerSet::get(const std::string& upstream) {
    auto it = breakers_.find(upstream);
    return it == breakers_.end() ? nullptr : it->second.get();
}

void CircuitBreakerSet::setStateCallback(const std::function<void(const std::string&, CircuitBreaker::State)>& cb) {
    for (auto& [name, breaker] : breakers_) {
        breaker->setStateCallback([name, cb](CircuitBreaker::State state) { cb(name, state); });
    }
}

}  // namespace gw::gateway
