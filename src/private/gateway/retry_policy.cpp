#include "gateway/retry_policy.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>

#include "base/random.hpp"

namespace gw::gateway {

namespace {

// 解析 HTTP-date（IMF-fixdate 等三种格式）
std::optional<std::time_t> parseHttpDate(std::string_view value) {
    std::string s(value);
    const char* formats[] = {"%a, %d %b %Y %H:%M:%S GMT", "%A, %d-%b-%y %H:%M:%S GMT", "%a %b %e %H:%M:%S %Y"};
    for (const char* fmt : formats) {
        std::tm tm{};
        if (::strptime(s.c_str(), fmt, &tm) != nullptr) {
            return ::timegm(&tm);
        }
    }
    return std::nullopt;
}

}  // namespace

FailureClass classifyTransport(TransportFailure kind) {
    switch (kind) {
        case TransportFailure::dns: return {true, true, false};
        case TransportFailure::connect_refused: return {true, true, false};
        case TransportFailure::connect_timeout: return {true, true, true};
        case TransportFailure::io_error: return {true, true, false};
        case TransportFailure::closed_before_head: return {true, true, true};
    }
    return {false, false, false};
}

FailureClass classifyHttpStatus(int status) {
    if (status == 408 || status == 409 || status == 425) return {true, false, true};
    if (status == 429) return {true, false, false};
    if (status >= 500 && status <= 599) return {true, true, false};
    return {false, false, false};  // 其他 4xx：直接透传
}

FailureClass classifyPrecommitStreamFailure() { return {true, true, false}; }
FailureClass classifyPostcommitStreamFailure() { return {false, true, false}; }

Duration backoffDelay(const RetryConfig& config, int attempt_in_target) {
    int n = std::max(attempt_in_target, 1);
    double base_ms = static_cast<double>(config.base_ms);
    double max_ms = static_cast<double>(config.max_ms);
    double delay_ms = std::min(max_ms, base_ms * static_cast<double>(1 << std::min(n - 1, 16)));
    double jitter = 0.8 + 0.4 * gw::randomDouble01();  // U(0.8, 1.2)
    delay_ms *= jitter;
    if (delay_ms < 1) delay_ms = 1;
    return std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::milli>(delay_ms));
}

std::optional<Duration> parseRetryAfter(std::string_view value, Timestamp now) {
    if (value.empty()) return std::nullopt;
    // 秒数
    std::int64_t seconds = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (ec == std::errc{} && ptr == value.data() + value.size() && seconds >= 0) {
        return std::chrono::seconds(seconds);
    }
    // HTTP-date
    auto when = parseHttpDate(value);
    if (!when) return std::nullopt;
    auto wall_now = std::chrono::system_clock::now();
    auto target = std::chrono::system_clock::from_time_t(*when);
    (void)now;
    if (target <= wall_now) return Duration::zero();
    return std::chrono::duration_cast<Duration>(target - wall_now);
}

std::optional<Duration> retryAfterFromHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers, Timestamp now) {
    for (const auto& [name, value] : headers) {
        if (name == "retry-after") return parseRetryAfter(value, now);
    }
    return std::nullopt;
}

}  // namespace gw::gateway
