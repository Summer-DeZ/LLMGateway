#include <gtest/gtest.h>

#include <chrono>

#include "gateway/token_bucket.hpp"

namespace {

using namespace std::chrono_literals;
using gw::Duration;
using gw::Timestamp;
using gw::gateway::TokenBucket;

// 假时钟：手动推进时间
class FakeClock : public gw::gateway::Clock {
public:
    Timestamp now() const override { return t_; }
    void advance(std::chrono::milliseconds d) { t_ += d; }

private:
    Timestamp t_{std::chrono::steady_clock::now()};
};

TEST(TokenBucketTest, InitialTokensEqualCapacity) {
    FakeClock clock;
    TokenBucket bucket(clock, 10.0, 1.0);
    EXPECT_DOUBLE_EQ(bucket.tokens(), 10.0);
    EXPECT_TRUE(bucket.canTake(10.0));
    // 桶满时超过容量的请求也能通过（policies.md §1.2）
    EXPECT_TRUE(bucket.canTake(10.5));
    bucket.take(10.5);
    EXPECT_FALSE(bucket.canTake(10.5));  // 不再满则不允许
}

TEST(TokenBucketTest, RefillAtConfiguredRate) {
    FakeClock clock;
    TokenBucket bucket(clock, 10.0, 1.0);
    bucket.take(10.0);  // 清空
    EXPECT_FALSE(bucket.canTake(1.0));
    clock.advance(2500ms);
    bucket.refill(clock.now());
    EXPECT_DOUBLE_EQ(bucket.tokens(), 2.5);
    EXPECT_TRUE(bucket.canTake(2.5));
    EXPECT_FALSE(bucket.canTake(2.6));
}

TEST(TokenBucketTest, RefillCappedAtCapacity) {
    FakeClock clock;
    TokenBucket bucket(clock, 10.0, 100.0);
    bucket.take(5.0);
    clock.advance(std::chrono::hours(1));
    bucket.refill(clock.now());
    EXPECT_DOUBLE_EQ(bucket.tokens(), 10.0);  // 满了就停，不会超过容量
}

TEST(TokenBucketTest, RequestLargerThanCapacityRequiresFullBucket) {
    FakeClock clock;
    TokenBucket bucket(clock, 10.0, 2.0);
    // 桶满时可以满足超过容量的请求（否则大请求永远无法通过）
    EXPECT_TRUE(bucket.canTake(15.0));
    bucket.take(15.0);
    EXPECT_DOUBLE_EQ(bucket.tokens(), -5.0);  // 负债
    // 不满时不允许
    EXPECT_FALSE(bucket.canTake(15.0));
}

TEST(TokenBucketTest, DebtRecoversWithRefill) {
    FakeClock clock;
    TokenBucket bucket(clock, 10.0, 1.0);
    bucket.take(15.0);  // -5
    EXPECT_FALSE(bucket.canTake(1.0));
    clock.advance(6000ms);
    bucket.refill(clock.now());
    EXPECT_DOUBLE_EQ(bucket.tokens(), 1.0);
    EXPECT_TRUE(bucket.canTake(1.0));
}

TEST(TokenBucketTest, AdjustClampsToCapacityAndNegativeCapacity) {
    FakeClock clock;
    TokenBucket bucket(clock, 10.0, 1.0);
    bucket.take(10.0);
    bucket.adjust(100.0);
    EXPECT_DOUBLE_EQ(bucket.tokens(), 10.0);  // 上限 capacity
    bucket.adjust(-100.0);
    EXPECT_DOUBLE_EQ(bucket.tokens(), -10.0);  // 下限 -capacity
}

TEST(TokenBucketTest, WaitForComputesTimeUntilCanTake) {
    FakeClock clock;
    TokenBucket bucket(clock, 10.0, 2.0);
    bucket.take(7.0);  // 剩 3
    // 需要再攒 4 个，速率 2/s → 2 秒
    EXPECT_EQ(bucket.waitFor(7.0), 2000ms);
    EXPECT_EQ(bucket.waitFor(3.0), Duration::zero());
    // 超过容量：等到桶满即可
    bucket.take(10.0);  // -7
    EXPECT_EQ(bucket.waitFor(15.0), 8500ms);  // 攒到满（10）需要 17/2 = 8.5s
}

TEST(TokenBucketTest, ZeroRateNeverRefills) {
    FakeClock clock;
    TokenBucket bucket(clock, 5.0, 0.0);
    bucket.take(5.0);
    clock.advance(std::chrono::hours(1));
    bucket.refill(clock.now());
    EXPECT_DOUBLE_EQ(bucket.tokens(), 0.0);
    EXPECT_FALSE(bucket.canTake(1.0));
}

}  // namespace
