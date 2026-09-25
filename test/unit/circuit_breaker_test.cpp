#include <gtest/gtest.h>

#include <chrono>

#include "gateway/circuit_breaker.hpp"

namespace {

using namespace std::chrono_literals;
using gw::gateway::CircuitBreaker;
using gw::gateway::Clock;
using gw::Timestamp;

class FakeClock : public Clock {
public:
    Timestamp now() const override { return t_; }
    void advance(std::chrono::milliseconds d) { t_ += d; }

private:
    Timestamp t_{std::chrono::steady_clock::now()};
};

gw::gateway::BreakerConfig cfg(int threshold = 3, std::chrono::milliseconds cooldown = 1000ms) {
    gw::gateway::BreakerConfig c;
    c.failure_threshold = threshold;
    c.cooldown = cooldown;
    return c;
}

TEST(CircuitBreakerTest, OpensAfterThresholdFailures) {
    FakeClock clock;
    CircuitBreaker breaker(cfg(), clock);
    EXPECT_EQ(breaker.state(), CircuitBreaker::State::closed);
    EXPECT_TRUE(breaker.allowRequest());
    breaker.recordFailure();
    breaker.recordFailure();
    EXPECT_EQ(breaker.state(), CircuitBreaker::State::closed);  // 还没到阈值
    breaker.recordFailure();
    EXPECT_EQ(breaker.state(), CircuitBreaker::State::open);
    EXPECT_FALSE(breaker.allowRequest());  // 冷却期间直接跳过
}

TEST(CircuitBreakerTest, SuccessResetsFailureCount) {
    FakeClock clock;
    CircuitBreaker breaker(cfg(), clock);
    breaker.recordFailure();
    breaker.recordFailure();
    breaker.recordSuccess();
    breaker.recordFailure();
    breaker.recordFailure();
    EXPECT_EQ(breaker.state(), CircuitBreaker::State::closed);  // 连续计数被成功重置
    breaker.recordFailure();
    EXPECT_EQ(breaker.state(), CircuitBreaker::State::open);
}

TEST(CircuitBreakerTest, HalfOpenAllowsSingleProbe) {
    FakeClock clock;
    CircuitBreaker breaker(cfg(2, 1000ms), clock);
    breaker.recordFailure();
    breaker.recordFailure();
    ASSERT_EQ(breaker.state(), CircuitBreaker::State::open);
    EXPECT_FALSE(breaker.allowRequest());
    clock.advance(999ms);
    EXPECT_FALSE(breaker.allowRequest());
    clock.advance(2ms);
    // 冷却结束：放行一个探测
    EXPECT_TRUE(breaker.allowRequest());
    EXPECT_EQ(breaker.state(), CircuitBreaker::State::half_open);
    // 已有探测在外：其他请求被拒
    EXPECT_FALSE(breaker.allowRequest());
    clock.advance(60000ms);
    EXPECT_FALSE(breaker.allowRequest());  // long-running probe still owns the slot
    // 探测成功 → closed
    breaker.recordSuccess();
    EXPECT_EQ(breaker.state(), CircuitBreaker::State::closed);
    EXPECT_TRUE(breaker.allowRequest());
}

TEST(CircuitBreakerTest, HalfOpenFailureDoublesCooldownWithCap) {
    FakeClock clock;
    CircuitBreaker breaker(cfg(1, 1000ms), clock);
    breaker.recordFailure();
    ASSERT_EQ(breaker.state(), CircuitBreaker::State::open);
    clock.advance(1001ms);
    ASSERT_TRUE(breaker.allowRequest());
    breaker.recordFailure();  // 探测失败：冷却翻倍为 2s
    EXPECT_EQ(breaker.state(), CircuitBreaker::State::open);
    clock.advance(1001ms);
    EXPECT_FALSE(breaker.allowRequest());  // 1s 不够，需要 2s
    clock.advance(1000ms);
    ASSERT_TRUE(breaker.allowRequest());
    breaker.recordFailure();  // 再次翻倍 4s
    clock.advance(2001ms);
    EXPECT_FALSE(breaker.allowRequest());
    clock.advance(2000ms);
    ASSERT_TRUE(breaker.allowRequest());
    breaker.recordFailure();
    // 连续翻倍直到 5 分钟上限
    for (int i = 0; i < 10; ++i) {
        clock.advance(6 * 60 * 1000ms);  // 远超上限
        ASSERT_TRUE(breaker.allowRequest()) << i;
        breaker.recordFailure();
    }
    clock.advance(5 * 60 * 1000ms + 1ms);
    // 上限 5 分钟：等待满 5 分钟后应能再探测（这里只验证不会无限增长）
    EXPECT_TRUE(breaker.allowRequest());
}

TEST(CircuitBreakerTest, ProbeReleaseOnCancellation) {
    FakeClock clock;
    CircuitBreaker breaker(cfg(1, 1000ms), clock);
    breaker.recordFailure();
    clock.advance(1001ms);
    ASSERT_TRUE(breaker.allowRequest());  // 占用探测名额
    EXPECT_FALSE(breaker.allowRequest());
    breaker.releaseProbe();  // 探测请求被取消
    EXPECT_TRUE(breaker.allowRequest());  // 名额释放，允许新的探测
}

TEST(CircuitBreakerSetTest, PerUpstreamIsolation) {
    auto config = gw::gateway::Config{};
    gw::gateway::UpstreamConfig a;
    a.name = "a";
    a.breaker = cfg(1, 1000ms);
    gw::gateway::UpstreamConfig b;
    b.name = "b";
    b.breaker = cfg(1, 1000ms);
    config.upstreams["a"] = a;
    config.upstreams["b"] = b;
    FakeClock clock;
    gw::gateway::CircuitBreakerSet set(config, clock);
    ASSERT_NE(set.get("a"), nullptr);
    ASSERT_NE(set.get("b"), nullptr);
    EXPECT_EQ(set.get("c"), nullptr);
    set.get("a")->recordFailure();
    EXPECT_EQ(set.get("a")->state(), CircuitBreaker::State::open);
    EXPECT_EQ(set.get("b")->state(), CircuitBreaker::State::closed);
}

}  // namespace
