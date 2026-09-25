#include <gtest/gtest.h>

#include "gateway/ledger.hpp"

namespace {

using namespace gw::gateway;

UsageRow sampleRow() {
    UsageRow row;
    row.ledger_id = "0123456789abcdef0123456789abcdef";
    row.client_request_id = "client-req-1";
    row.gateway_instance = "gw-1";
    row.key_name = "dagent";
    row.billing_day = "2026-09-25";
    row.started_at_ms = 1790337600000LL;
    row.finished_at_ms = 1790337612345LL;
    row.ingress = "openai";
    row.model = "qwen";
    row.upstream = "qwen36";
    row.stream = true;
    row.outcome = "ok";
    row.http_status = 200;
    row.input_tokens = 100;
    row.cached_tokens = 40;
    row.computed_input_tokens = 60;
    row.output_tokens = 500;
    row.billable_tokens = 600;
    row.usage_complete = true;
    row.cost_micro_usd = 206;  // 60×0.1 + 40×0.1 + 500×0.4
    row.truncated = false;
    row.latency_ms = 12345;
    row.ttft_ms = 96;
    row.attempts = nlohmann::json::array({
        nlohmann::json{{"upstream", "qwen36"}, {"result", "ok"}, {"completion", 500}},
    });
    return row;
}

std::optional<std::string> param(const std::vector<std::optional<std::string>>& p, std::size_t i) {
    return i < p.size() ? p[i] : std::optional<std::string>(std::nullopt);
}

TEST(UsageRowTest, ToParamsFullRecord) {
    auto p = sampleRow().toParams();
    ASSERT_EQ(p.size(), 24u);
    EXPECT_EQ(*param(p, 0), "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(*param(p, 1), "client-req-1");
    EXPECT_EQ(*param(p, 2), "gw-1");
    EXPECT_EQ(*param(p, 3), "dagent");
    EXPECT_EQ(*param(p, 4), "2026-09-25");
    // timestamptz 文本（UTC）
    EXPECT_EQ(*param(p, 5), "2026-09-25 12:00:00.000+00");
    EXPECT_EQ(*param(p, 6), "2026-09-25 12:00:12.345+00");
    EXPECT_EQ(*param(p, 7), "openai");
    EXPECT_EQ(*param(p, 8), "qwen");
    EXPECT_EQ(*param(p, 9), "qwen36");
    EXPECT_EQ(*param(p, 10), "true");
    EXPECT_EQ(*param(p, 11), "ok");
    EXPECT_EQ(*param(p, 12), "200");
    EXPECT_EQ(*param(p, 13), "100");
    EXPECT_EQ(*param(p, 14), "40");
    EXPECT_EQ(*param(p, 15), "60");
    EXPECT_EQ(*param(p, 16), "500");
    EXPECT_EQ(*param(p, 17), "600");
    EXPECT_EQ(*param(p, 18), "true");
    EXPECT_EQ(*param(p, 19), "206");
    EXPECT_EQ(*param(p, 20), "false");
    EXPECT_EQ(*param(p, 21), "12345");
    EXPECT_EQ(*param(p, 22), "96");
    EXPECT_TRUE(param(p, 23)->find("qwen36") != std::string::npos);
}

TEST(UsageRowTest, NullFieldsAreNullParams) {
    auto row = sampleRow();
    row.input_tokens = std::nullopt;  // 上游未报告
    row.output_tokens = std::nullopt;
    row.cost_micro_usd = std::nullopt;
    row.upstream = "";      // 空串 → NULL
    row.client_request_id = "";
    row.ttft_ms = std::nullopt;
    auto p = row.toParams();
    EXPECT_FALSE(param(p, 1).has_value());   // client_request_id
    EXPECT_FALSE(param(p, 9).has_value());   // upstream
    EXPECT_FALSE(param(p, 13).has_value());  // input_tokens
    EXPECT_FALSE(param(p, 16).has_value());  // output_tokens
    EXPECT_FALSE(param(p, 19).has_value());  // cost
    EXPECT_FALSE(param(p, 22).has_value());  // ttft
    // billable 与 usage_complete 不受影响（consumed 为 null 的尝试按 0 计）
    EXPECT_EQ(*param(p, 17), "600");
    EXPECT_EQ(*param(p, 18), "true");
}

TEST(UsageRowTest, CostIsIntegerMicroUsd) {
    auto row = sampleRow();
    // 大成本：0.0000005 美元级别不出现，价格按百万分位整数化（四舍五入）
    row.computed_input_tokens = 3;
    row.cached_tokens = 7;
    row.output_tokens = 111;
    row.cost_micro_usd = 3 * 100 + 7 * 100 + 111 * 400;  // 手算 45400 micro
    EXPECT_EQ(*row.toParams()[19], "45400");
}

TEST(UsageRowTest, BillableIsSumOfConsumed) {
    auto row = sampleRow();
    // cached(40) + computed(60) + output(500) = 600
    EXPECT_EQ(row.billable_tokens, 600);
}

TEST(UsageRowTest, AttemptsSerializedAsJson) {
    auto p = sampleRow().toParams();
    auto attempts = nlohmann::json::parse(*param(p, 23));
    ASSERT_TRUE(attempts.is_array());
    ASSERT_EQ(attempts.size(), 1u);
    EXPECT_EQ(attempts[0]["upstream"], "qwen36");
    EXPECT_EQ(attempts[0]["completion"], 500);
}

TEST(UsageRowTest, EmptyAttemptsJsonArray) {
    auto row = sampleRow();
    row.attempts = nlohmann::json::array();
    auto p = row.toParams();
    EXPECT_EQ(*param(p, 23), "[]");
}

}  // namespace
