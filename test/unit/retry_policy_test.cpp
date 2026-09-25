#include <gtest/gtest.h>

#include <chrono>

#include "gateway/retry_policy.hpp"
#include "http/http_request.hpp"

namespace {

using namespace std::chrono_literals;
using gw::gateway::FailureClass;
using gw::gateway::RetryConfig;
using gw::gateway::TransportFailure;

TEST(RetryPolicyTest, FailureClassificationTable) {
    // proxy-session.md §6.1 逐项
    auto dns = gw::gateway::classifyTransport(TransportFailure::dns);
    EXPECT_TRUE(dns.retryable);
    EXPECT_TRUE(dns.counts_toward_breaker);

    auto refused = gw::gateway::classifyTransport(TransportFailure::connect_refused);
    EXPECT_TRUE(refused.retryable);
    EXPECT_TRUE(refused.counts_toward_breaker);

    auto connect_timeout = gw::gateway::classifyTransport(TransportFailure::connect_timeout);
    EXPECT_TRUE(connect_timeout.retryable);
    EXPECT_TRUE(connect_timeout.counts_toward_breaker);
    EXPECT_TRUE(connect_timeout.is_timeout);

    auto io_error = gw::gateway::classifyTransport(TransportFailure::io_error);
    EXPECT_TRUE(io_error.retryable);
    EXPECT_TRUE(io_error.counts_toward_breaker);

    auto before_head = gw::gateway::classifyTransport(TransportFailure::closed_before_head);
    EXPECT_TRUE(before_head.retryable);
    EXPECT_TRUE(before_head.counts_toward_breaker);

    // HTTP 状态
    for (int status : {408, 409, 425}) {
        auto c = gw::gateway::classifyHttpStatus(status);
        EXPECT_TRUE(c.retryable) << status;
        EXPECT_FALSE(c.counts_toward_breaker) << status;  // 可重试但不计熔断
    }
    {
        auto c = gw::gateway::classifyHttpStatus(429);
        EXPECT_TRUE(c.retryable);
        EXPECT_FALSE(c.counts_toward_breaker);  // 上游配额问题，不是故障
    }
    for (int status : {500, 502, 503, 504, 529}) {
        auto c = gw::gateway::classifyHttpStatus(status);
        EXPECT_TRUE(c.retryable) << status;
        EXPECT_TRUE(c.counts_toward_breaker) << status;
    }
    for (int status : {400, 401, 403, 404, 413, 422}) {
        auto c = gw::gateway::classifyHttpStatus(status);
        EXPECT_FALSE(c.retryable) << status;  // 直接透传
        EXPECT_FALSE(c.counts_toward_breaker) << status;
    }

    // 2xx 提交前流中断 / 流内 error：可重试且计入熔断
    auto pre = gw::gateway::classifyPrecommitStreamFailure();
    EXPECT_TRUE(pre.retryable);
    EXPECT_TRUE(pre.counts_toward_breaker);
    // 提交后流中断 / idle：不可重试，计入熔断
    auto post = gw::gateway::classifyPostcommitStreamFailure();
    EXPECT_FALSE(post.retryable);
    EXPECT_TRUE(post.counts_toward_breaker);
}

TEST(RetryPolicyTest, BackoffBoundsAndExponentialGrowth) {
    RetryConfig cfg;
    cfg.base_ms = 500;
    cfg.max_ms = 8000;
    // 指数增长且带 ±20% 抖动
    for (int attempt = 1; attempt <= 5; ++attempt) {
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
            gw::gateway::backoffDelay(cfg, attempt));
        double base = std::min<double>(cfg.max_ms, cfg.base_ms * (1 << (attempt - 1)));
        EXPECT_GE(static_cast<double>(delay.count()), base * 0.8 - 1) << attempt;
        EXPECT_LE(static_cast<double>(delay.count()), base * 1.2 + 1) << attempt;
    }
    // 上限
    auto capped = std::chrono::duration_cast<std::chrono::milliseconds>(gw::gateway::backoffDelay(cfg, 20));
    EXPECT_LE(capped.count(), static_cast<std::int64_t>(cfg.max_ms * 1.2) + 1);
}

TEST(RetryPolicyTest, ParseRetryAfterSeconds) {
    auto now = gw::nowSteady();
    EXPECT_EQ(gw::gateway::parseRetryAfter("1", now), 1s);
    EXPECT_EQ(gw::gateway::parseRetryAfter("60", now), 60s);
    EXPECT_EQ(gw::gateway::parseRetryAfter("0", now), 0s);
    EXPECT_FALSE(gw::gateway::parseRetryAfter("abc", now).has_value());
    EXPECT_FALSE(gw::gateway::parseRetryAfter("-5", now).has_value());
    EXPECT_FALSE(gw::gateway::parseRetryAfter("", now).has_value());
}

TEST(RetryPolicyTest, ParseRetryAfterHttpDate) {
    auto now = gw::nowSteady();
    // 过去的时间 → 0（立刻可重试）
    auto past = gw::gateway::parseRetryAfter("Wed, 21 Oct 2015 07:28:00 GMT", now);
    ASSERT_TRUE(past.has_value());
    EXPECT_EQ(*past, gw::Duration::zero());

    // 未来 60 秒左右
    std::time_t future = std::time(nullptr) + 60;
    std::tm tm{};
    gmtime_r(&future, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    auto when = gw::gateway::parseRetryAfter(buf, now);
    ASSERT_TRUE(when.has_value());
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(*when).count();
    EXPECT_GE(seconds, 55);
    EXPECT_LE(seconds, 65);
}

TEST(RetryPolicyTest, RetryAfterFromHeaders) {
    gw::http::Headers headers{{"content-type", "application/json"}, {"retry-after", "3"}};
    auto value = gw::gateway::retryAfterFromHeaders(headers, gw::nowSteady());
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 3s);
    gw::http::Headers none{{"content-type", "application/json"}};
    EXPECT_FALSE(gw::gateway::retryAfterFromHeaders(none, gw::nowSteady()).has_value());
}

}  // namespace
