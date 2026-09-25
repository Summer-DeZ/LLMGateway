#include "gateway/request_record.hpp"

namespace gw::gateway {

namespace {

nlohmann::json optInt(std::optional<std::int64_t> v) {
    if (!v) return nullptr;
    return *v;
}

}  // namespace

const char* usageSourceToString(AttemptUsage::Source source) {
    switch (source) {
        case AttemptUsage::Source::none: return "none";
        case AttemptUsage::Source::rejected: return "rejected";
        case AttemptUsage::Source::upstream_final: return "upstream_final";
        case AttemptUsage::Source::upstream_partial: return "upstream_partial";
        case AttemptUsage::Source::unreported: return "unreported";
    }
    return "none";
}

nlohmann::json AttemptRecord::toJson() const {
    nlohmann::json j;
    j["upstream"] = upstream;
    j["result"] = result;
    j["http_status"] = http_status;
    j["prompt"] = optInt(usage.prompt);
    j["cached"] = optInt(usage.cached);
    j["prompt_computed"] = optInt(usage.prompt_computed);
    j["completion"] = optInt(usage.completion);
    j["source"] = usageSourceToString(usage.source);
    return j;
}

nlohmann::json RequestRecord::toJson() const {
    nlohmann::json j;
    j["ts"] = ts;
    j["request_id"] = request_id;
    j["user"] = user;
    j["ingress"] = ingress;
    j["model"] = model;
    j["stream"] = stream;
    j["upstream"] = upstream;
    j["upstream_kind"] = upstream_kind;
    j["attempts"] = attempts;
    j["fallback"] = fallback;
    j["outcome"] = outcome;
    j["http_status"] = http_status;
    j["latency_ms"] = latency_ms;
    j["ttft_ms"] = ttft_ms ? nlohmann::json(*ttft_ms) : nlohmann::json(nullptr);
    j["upstream_ttfb_ms"] = upstream_ttfb_ms ? nlohmann::json(*upstream_ttfb_ms) : nlohmann::json(nullptr);
    j["connect_ms"] = connect_ms ? nlohmann::json(*connect_ms) : nlohmann::json(nullptr);
    j["conn_reused"] = conn_reused ? nlohmann::json(*conn_reused) : nlohmann::json(nullptr);
    j["prompt_tokens"] = optInt(prompt_tokens);
    j["cached_tokens"] = optInt(cached_tokens);
    j["prompt_computed_tokens"] = optInt(prompt_computed_tokens);
    j["completion_tokens"] = optInt(completion_tokens);
    j["usage_source"] = usage_source;
    j["usage_complete"] = usage_complete;
    nlohmann::json attempts_json = nlohmann::json::array();
    for (const auto& attempt : attempt_list) attempts_json.push_back(attempt.toJson());
    j["attempts"] = std::move(attempts_json);
    j["cost_usd"] = cost_usd ? nlohmann::json(*cost_usd) : nlohmann::json(nullptr);
    j["saved_cost_usd"] = saved_cost_usd ? nlohmann::json(*saved_cost_usd) : nlohmann::json(nullptr);
    j["cache"] = cache;
    j["backpressure_pauses"] = backpressure_pauses;
    j["backpressure_ms"] = backpressure_ms;
    j["cancel_latency_us"] = cancel_latency_us ? nlohmann::json(*cancel_latency_us) : nlohmann::json(nullptr);
    j["error"] = error;
    return j;
}

}  // namespace gw::gateway
