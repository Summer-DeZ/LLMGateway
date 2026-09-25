#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "gateway/rate_limiter.hpp"

namespace {

using namespace std::chrono_literals;
using gw::Duration;
using gw::Timestamp;
using gw::gateway::Clock;
using gw::gateway::Config;
using gw::gateway::ModelConfig;
using gw::gateway::RateLimiter;
using gw::gateway::UserConfig;

class FakeClock : public Clock {
public:
    Timestamp now() const override { return t_; }
    void advance(std::chrono::milliseconds d) { t_ += d; }

private:
    Timestamp t_{std::chrono::steady_clock::now()};
};

Config testConfig() {
    Config cfg;
    UserConfig u;
    u.key = "k1";
    u.name = "tester";
    u.rpm = 4;    // 容量 4，补充 4/60 每秒
    u.tpm = 1000;
    cfg.users["k1"] = u;
    ModelConfig m;
    m.alias = "m";
    m.targets = {"x"};
    m.rpm = 10;
    m.tpm = 600;
    cfg.models["m"] = m;
    return cfg;
}

const UserConfig& user(const Config& cfg) { return cfg.users.at("k1"); }
const ModelConfig& model(const Config& cfg) { return cfg.models.at("m"); }

TEST(RateLimiterTest, AcquireDeductsAllFourBuckets) {
    FakeClock clock;
    auto cfg = testConfig();
    RateLimiter rl(cfg, clock);
    auto r = rl.tryAcquire(user(cfg), model(cfg), 100.0);
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*rl.userTokens("k1"), 900.0);
    EXPECT_DOUBLE_EQ(*rl.modelTokens("m"), 500.0);
    // 快照取两维度中较紧的
    EXPECT_DOUBLE_EQ(*(*r)->limit_requests, 4.0);
    EXPECT_DOUBLE_EQ(*(*r)->remaining_requests, 3.0);
    EXPECT_DOUBLE_EQ(*(*r)->limit_tokens, 600.0);
    EXPECT_DOUBLE_EQ(*(*r)->remaining_tokens, 500.0);
}

TEST(RateLimiterTest, AnyInsufficientRejectsAllWithoutDeduction) {
    FakeClock clock;
    auto cfg = testConfig();
    RateLimiter rl(cfg, clock);
    // 第一次扣掉 user.tpm 100（model.tpm 600→500）
    ASSERT_TRUE(rl.tryAcquire(user(cfg), model(cfg), 100.0).has_value());
    // 第二次要 550：user.tpm 900 够、model.tpm 500 不够 → 整体拒绝
    auto r = rl.tryAcquire(user(cfg), model(cfg), 550.0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().scope, "model");
    EXPECT_EQ(r.error().dimension, "tokens");
    // 没有扣任何桶：user.tpm 仍是 900
    EXPECT_DOUBLE_EQ(*rl.userTokens("k1"), 900.0);
    // 拒绝后再来一个小请求，正常通过（说明确实没扣）
    ASSERT_TRUE(rl.tryAcquire(user(cfg), model(cfg), 100.0).has_value());
    EXPECT_DOUBLE_EQ(*rl.userTokens("k1"), 800.0);
}

TEST(RateLimiterTest, RequestDimensionRejectionHasRetryAfter) {
    FakeClock clock;
    auto cfg = testConfig();
    RateLimiter rl(cfg, clock);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(rl.tryAcquire(user(cfg), model(cfg), 1.0).has_value());
    }
    // user.rpm 用尽（4 个），model.rpm 还剩 6 → 拒绝 scope=user dimension=requests
    auto r = rl.tryAcquire(user(cfg), model(cfg), 1.0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().scope, "user");
    EXPECT_EQ(r.error().dimension, "requests");
    // 补充速率 4/60 每秒 → 等 15 秒
    EXPECT_EQ(r.error().retry_after, 15s);
}

TEST(RateLimiterTest, SettleRefundsDifference) {
    FakeClock clock;
    auto cfg = testConfig();
    RateLimiter rl(cfg, clock);
    auto ticket = rl.tryAcquire(user(cfg), model(cfg), 100.0);
    ASSERT_TRUE(ticket.has_value());
    // 实际只用了 30：退还 70
    rl.settle(**ticket, 30);
    EXPECT_DOUBLE_EQ(*rl.userTokens("k1"), 970.0);
    EXPECT_DOUBLE_EQ(*rl.modelTokens("m"), 570.0);
}

TEST(RateLimiterTest, SettleOverrunGoesIntoDebt) {
    FakeClock clock;
    auto cfg = testConfig();
    RateLimiter rl(cfg, clock);
    auto ticket = rl.tryAcquire(user(cfg), model(cfg), 100.0);
    ASSERT_TRUE(ticket.has_value());
    // 实际用了 250：补扣 150（负债）
    rl.settle(**ticket, 250);
    EXPECT_DOUBLE_EQ(*rl.userTokens("k1"), 750.0);
    EXPECT_DOUBLE_EQ(*rl.modelTokens("m"), 350.0);
}

TEST(RateLimiterTest, SettleWithUnknownUsageKeepsReservation) {
    FakeClock clock;
    auto cfg = testConfig();
    RateLimiter rl(cfg, clock);
    auto ticket = rl.tryAcquire(user(cfg), model(cfg), 100.0);
    ASSERT_TRUE(ticket.has_value());
    rl.settle(**ticket, std::nullopt);  // 未报告：不退还
    EXPECT_DOUBLE_EQ(*rl.userTokens("k1"), 900.0);
    EXPECT_DOUBLE_EQ(*rl.modelTokens("m"), 500.0);
}

TEST(RateLimiterTest, UnlimitedDimensionsPassThrough) {
    FakeClock clock;
    Config cfg;
    UserConfig u;
    u.key = "k";
    u.name = "n";  // rpm/tpm 均为 0：不限
    cfg.users["k"] = u;
    ModelConfig m;
    m.alias = "a";  // 不限
    cfg.models["a"] = m;
    RateLimiter rl(cfg, clock);
    for (int i = 0; i < 100; ++i) {
        auto r = rl.tryAcquire(cfg.users.at("k"), cfg.models.at("a"), 1e9);
        ASSERT_TRUE(r.has_value());
        EXPECT_FALSE((*r)->limit_requests.has_value());
        EXPECT_FALSE((*r)->limit_tokens.has_value());
    }
}

TEST(RateLimiterTest, ConcurrentAcquiresDeductExactly) {
    auto cfg = testConfig();
    // 巨大容量 + 近零补充：16 线程 × 1000 次扣减总数可以精确核对（60ms 内补充量 ≈ 1e-10）
    cfg.users["k1"].rpm = 1e-9;
    cfg.users["k1"].rpm_burst = 1e12;
    cfg.users["k1"].tpm = 1e-9;
    cfg.users["k1"].tpm_burst = 1e12;
    cfg.models["m"].rpm = 1e-9;
    cfg.models["m"].rpm_burst = 1e12;
    cfg.models["m"].tpm = 1e-9;
    cfg.models["m"].tpm_burst = 1e12;
    gw::gateway::SteadyClock clock;
    RateLimiter rl(cfg, clock);

    constexpr int kThreads = 16;
    constexpr int kPerThread = 1000;
    std::atomic<int> admitted{0};
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i) {
                    if (rl.tryAcquire(cfg.users.at("k1"), cfg.models.at("m"), 10.0).has_value()) {
                        admitted.fetch_add(1);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    EXPECT_EQ(admitted.load(), kThreads * kPerThread);
    EXPECT_DOUBLE_EQ(*rl.userTokens("k1"), 1e12 - 16000.0 * 10.0);
    EXPECT_DOUBLE_EQ(*rl.modelTokens("m"), 1e12 - 16000.0 * 10.0);
}

}  // namespace
