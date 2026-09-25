#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "gateway/config.hpp"
#include "gateway/token_bucket.hpp"  // Clock

namespace gw::gateway {

// 每个上游一个，跨线程共享（内部 mutex）。状态机见 proxy-session.md §6.4：
// closed --failure_threshold 次连续失败--> open --cooldown--> half_open（只放行一个探测）
//     --成功--> closed；--失败--> open 且冷却翻倍（上限 5 分钟）
class CircuitBreaker : noncopyable {
public:
    enum class State { closed, open, half_open };

    CircuitBreaker(const BreakerConfig& config, const Clock& clock);

    // 尝试计划用：open 且未到冷却 → 拒绝；冷却到期 → 进入 half_open 并占用唯一探测名额；
    // half_open 已有探测在外 → 拒绝（探测超时未回也按冷却释放）
    bool allowRequest();
    void recordSuccess();
    void recordFailure();
    // 探测请求被取消等没有结论的情况：释放 half_open 的探测名额，状态保持 half_open
    void releaseProbe();

    State state() const;
    void setStateCallback(std::function<void(State)> cb);

    static const char* stateName(State state);

private:
    void toOpen(Timestamp now, bool double_cooldown);

    mutable std::mutex mutex_;
    BreakerConfig config_;
    const Clock* clock_;
    State state_ = State::closed;
    int consecutive_failures_ = 0;
    Timestamp opened_at_{};
    std::chrono::milliseconds current_cooldown_{0};
    Timestamp probe_started_at_{};
    bool probe_outstanding_ = false;
    std::function<void(State)> state_callback_;
};

// 按上游名预建（启动时按配置注册，运行期只读查找）
class CircuitBreakerSet : noncopyable {
public:
    CircuitBreakerSet(const Config& config, const Clock& clock);

    CircuitBreaker* get(const std::string& upstream);
    void setStateCallback(const std::function<void(const std::string&, CircuitBreaker::State)>& cb);

private:
    std::unordered_map<std::string, std::unique_ptr<CircuitBreaker>> breakers_;
};

}  // namespace gw::gateway
