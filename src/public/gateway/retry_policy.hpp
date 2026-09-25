#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "base/timestamp.hpp"
#include "gateway/config.hpp"

namespace gw::gateway {

// 失败分类（proxy-session.md §6.1）
struct FailureClass {
    bool retryable = false;
    bool counts_toward_breaker = false;
    bool is_timeout = false;  // 最后一次失败是超时 → 最终错误 504；否则传输失败 → 502
};

enum class TransportFailure { dns, connect_refused, connect_timeout, io_error, closed_before_head };

FailureClass classifyTransport(TransportFailure kind);
FailureClass classifyHttpStatus(int status);
// 2xx 但提交前流中断 / 首事件前流内出现 error 事件
FailureClass classifyPrecommitStreamFailure();
// 提交后流中断 / idle 超时
FailureClass classifyPostcommitStreamFailure();

// 退避：delay = min(max_ms, base_ms × 2^(n-1)) × U(0.8, 1.2)；有 Retry-After 时 max(delay, retry_after)。
// n 为当前目标内第几次尝试（1-based：第 1 次失败后 n=1 → base_ms）。
Duration backoffDelay(const RetryConfig& config, int attempt_in_target);
// Retry-After：秒数或 HTTP-date；非法/过去的时间返回 0
std::optional<Duration> parseRetryAfter(std::string_view value, Timestamp now);

// 从上游响应头取 Retry-After（名字已转小写）
std::optional<Duration> retryAfterFromHeaders(const std::vector<std::pair<std::string, std::string>>& headers,
                                              Timestamp now);

}  // namespace gw::gateway
