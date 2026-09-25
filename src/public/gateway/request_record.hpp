#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/usage_meter.hpp"

namespace gw::gateway {

// 一次尝试的计量与结果（policies.md §3.1 attempts[]）
struct AttemptRecord {
    std::string upstream;
    std::string kind;
    std::string result;  // ok | retryable | fatal
    int http_status = 0;
    AttemptUsage usage;

    nlohmann::json toJson() const;
};

struct RequestRecord {
    std::int64_t ts = 0;  // 请求收齐时刻（UTC 毫秒）
    std::string request_id;
    std::string user;  // users.*.name
    std::string ingress;  // openai | anthropic
    std::string model;  // 别名
    bool stream = false;
    std::string upstream;  // 最终服务的上游
    std::string upstream_kind;
    int attempts = 0;
    bool fallback = false;
    std::string outcome;
    int http_status = 0;
    std::int64_t latency_ms = 0;
    std::optional<std::int64_t> ttft_ms;  // 流式；非流式为 null
    std::optional<std::int64_t> upstream_ttfb_ms;
    std::optional<std::int64_t> connect_ms;
    std::optional<bool> conn_reused;
    // 各尝试之和，全部来自上游报告；有未报告部分时为 null
    std::optional<std::int64_t> prompt_tokens;
    std::optional<std::int64_t> cached_tokens;
    std::optional<std::int64_t> prompt_computed_tokens;
    std::optional<std::int64_t> completion_tokens;
    std::string usage_source;  // upstream_final | upstream_partial | unreported | cache | rejected
    bool usage_complete = false;
    std::vector<AttemptRecord> attempt_list;
    std::optional<double> cost_usd;
    std::optional<double> saved_cost_usd;
    std::string cache;  // hit | miss | none
    int backpressure_pauses = 0;
    std::int64_t backpressure_ms = 0;
    std::optional<std::int64_t> cancel_latency_us;
    std::string error;

    nlohmann::json toJson() const;
};

const char* usageSourceToString(AttemptUsage::Source source);

}  // namespace gw::gateway
