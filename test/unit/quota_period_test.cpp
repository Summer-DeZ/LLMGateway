#include <gtest/gtest.h>

#include "gateway/quota.hpp"

namespace {

using namespace gw::gateway;

// 用固定 epoch 计算，避免依赖当前时间
// 2026-09-25 12:00:00 UTC
constexpr std::int64_t kNoonUtcMs = 1790337600LL * 1000;
// 2026-09-25 15:59:59 UTC（+8 已过零点：23:59:59 本地）
constexpr std::int64_t kBeforeMidnightLocalMs = 1790351999LL * 1000;

TEST(QuotaPeriodTest, DayAndMonthKeysWithUtcOffset) {
    // 默认 +08:00：2026-09-25 12:00 UTC = 本地 2026-09-25 20:00
    EXPECT_EQ(quotaDayOf(kNoonUtcMs, 480), 20260925);
    EXPECT_EQ(quotaMonthOf(kNoonUtcMs, 480), 202609);
    EXPECT_EQ(quotaBillingDay(kNoonUtcMs, 480), "2026-09-25");

    // UTC+0
    EXPECT_EQ(quotaDayOf(kNoonUtcMs, 0), 20260925);

    // UTC-10：本地 02:00 → 还是 25 日
    EXPECT_EQ(quotaDayOf(kNoonUtcMs, -600), 20260925);
}

TEST(QuotaPeriodTest, DayKeyRollsOverAtLocalMidnight) {
    // 2026-09-25 15:59:59 UTC（+8 → 本地 23:59:59，25 日）
    EXPECT_EQ(quotaDayOf(kBeforeMidnightLocalMs, 480), 20260925);
    // 1 秒后（本地零点跨越）→ 26 日
    std::int64_t after_midnight = kBeforeMidnightLocalMs + 1000;
    EXPECT_EQ(quotaDayOf(after_midnight, 480), 20260926);
    EXPECT_EQ(quotaBillingDay(after_midnight, 480), "2026-09-26");
}

TEST(QuotaPeriodTest, MonthKeyCrossesMonthBoundary) {
    // 2026-09-30 16:00:00 UTC（+8 → 本地 10-01 00:00）→ 月键应为 202610
    std::int64_t ms = 1790784000LL * 1000;
    EXPECT_EQ(quotaDayOf(ms, 480), 20261001);
    EXPECT_EQ(quotaMonthOf(ms, 480), 202610);
    // 界前一秒仍是 9 月
    std::int64_t before = ms - 1000;
    EXPECT_EQ(quotaDayOf(before, 480), 20260930);
    EXPECT_EQ(quotaMonthOf(before, 480), 202609);
}

TEST(QuotaPeriodTest, KeyNamesUseHashTag) {
    EXPECT_EQ(quotaDayKey("dagent", 20260925), "gw:quota:{dagent}:d:20260925");
    EXPECT_EQ(quotaMonthKey("dagent", 202609), "gw:quota:{dagent}:m:202609");
}

TEST(QuotaPeriodTest, RetryAfterToNextLocalMidnight) {
    // 2026-09-25 12:00 UTC（+8 → 本地 20:00）→ 距本地零点 4 小时
    EXPECT_EQ(secondsToNextLocalMidnight(kNoonUtcMs, 480), 4 * 3600);
    // 23:59:59 → 1 秒
    EXPECT_EQ(secondsToNextLocalMidnight(kBeforeMidnightLocalMs, 480), 1);
    // UTC+0：12:00 → 12 小时
    EXPECT_EQ(secondsToNextLocalMidnight(kNoonUtcMs, 0), 12 * 3600);
}

TEST(QuotaPeriodTest, QuotaViewOverBudgetUsesProjected) {
    QuotaView view;
    view.remaining = 1000;
    view.reported = 0;
    view.projected = 999;
    EXPECT_FALSE(view.overBudget());
    view.projected = 1001;
    EXPECT_TRUE(view.overBudget());
    // remaining 不限（nullopt）永不超额
    view.remaining = std::nullopt;
    view.projected = 1 << 30;
    EXPECT_FALSE(view.overBudget());
}

TEST(QuotaPeriodTest, QuotaViewShouldReport) {
    QuotaView view;
    view.consumed = 0;
    view.reported = 0;
    view.last_report_ms = 0;
    // 攒够 report_every_tokens → 上报
    view.consumed = 32;
    EXPECT_TRUE(view.shouldReport(100, 32, 100));
    // 没攒够但超过间隔 → 上报
    view.consumed = 10;
    EXPECT_FALSE(view.shouldReport(99, 32, 100));
    EXPECT_TRUE(view.shouldReport(101, 32, 100));
    // 已全部上报且未到间隔 → 不上报
    view.reported = 10;
    view.last_report_ms = 90;
    EXPECT_FALSE(view.shouldReport(101, 32, 100));
    // 超过间隔 → 上报
    EXPECT_TRUE(view.shouldReport(191, 32, 100));
}

}  // namespace
