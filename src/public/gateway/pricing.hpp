#pragma once

#include <cstdint>
#include <optional>

#include "gateway/config.hpp"
#include "gateway/usage_meter.hpp"

namespace gw::gateway {

// 单次尝试的成本，单位百万分之一美元（整数，避免浮点累加误差）。
// cost = (prompt_computed × input + cached × cached_price + completion × output) / 1e6（policies.md §4.6）
// 任何字段未报告 → nullopt（不按部分计算）；上游没有配置单价 → 0；cached 单价缺省时等于 input。
std::optional<std::int64_t> attemptCostMicroUsd(const PricingEntry* pricing, const AttemptUsage& usage);

inline double microUsdToUsd(std::int64_t micro_usd) {
    return static_cast<double>(micro_usd) / 1e6;
}

}  // namespace gw::gateway
