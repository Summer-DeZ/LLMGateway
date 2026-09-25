#include "gateway/pricing.hpp"

namespace gw::gateway {

std::optional<std::int64_t> attemptCostMicroUsd(const PricingEntry* pricing, const AttemptUsage& usage) {
    if (pricing == nullptr) return 0;
    if (!usage.prompt_computed || !usage.cached || !usage.completion) return std::nullopt;
    double input = pricing->input.value_or(0.0);
    double output = pricing->output.value_or(0.0);
    double cached_price = pricing->cached.value_or(input);
    // 单价是"美元/百万 token"：cost/1e6 美元 = cost 微美元
    double cost = static_cast<double>(*usage.prompt_computed) * input + static_cast<double>(*usage.cached) * cached_price +
                  static_cast<double>(*usage.completion) * output;
    return static_cast<std::int64_t>(cost + 0.5);
}

}  // namespace gw::gateway
