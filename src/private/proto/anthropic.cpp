#include "proto/anthropic.hpp"

namespace gw::proto {
std::expected<OpenAiRequestMeta, std::string> parseAnthropicRequest(const nlohmann::json& body) {
    auto meta = parseOpenAiRequest(body);
    if (meta) meta->client_wants_usage = true;
    return meta;
}
OpenAiUsageView extractAnthropicUsage(const nlohmann::json& body) {
    OpenAiUsageView result;
    if (!body.contains("usage") || !body["usage"].is_object()) return result;
    const auto& u = body["usage"];
    result.present = u.contains("input_tokens") && u.contains("output_tokens");
    result.cached = u.value("cache_read_input_tokens", std::int64_t{0});
    result.prompt = u.value("input_tokens", std::int64_t{0}) + *result.cached +
                    u.value("cache_creation_input_tokens", std::int64_t{0});
    result.completion = u.value("output_tokens", std::int64_t{0});
    return result;
}
void observeAnthropicUsage(const nlohmann::json& event, gateway::UsageMeter& meter) {
    const auto type = event.value("type", "");
    if (type == "message_start") {
        const auto& u = event.at("message").at("usage");
        meter.onAnthropicMessageStart(u.value("input_tokens", std::int64_t{0}),
                                      u.value("cache_read_input_tokens", std::int64_t{0}),
                                      u.value("cache_creation_input_tokens", std::int64_t{0}));
        if (u.contains("output_tokens")) meter.onAnthropicMessageDelta(u.at("output_tokens").get<std::int64_t>());
    } else if (type == "message_delta" && event.contains("usage")) {
        const auto& u = event.at("usage");
        if (u.contains("input_tokens"))
            meter.onAnthropicMessageStart(u.at("input_tokens").get<std::int64_t>(),
                                          u.value("cache_read_input_tokens", meter.value().cached.value_or(0)),
                                          u.value("cache_creation_input_tokens", std::int64_t{0}));
        if (u.contains("output_tokens")) meter.onAnthropicMessageDelta(u.at("output_tokens").get<std::int64_t>());
        meter.markFinalUsage();
    }
}
void AnthropicStreamState::observe(const nlohmann::json& e) {
    auto type = e.value("type", "");
    if (type == "message_start") started_ = true;
    if (type == "message_stop") done_ = true;
    if (type == "content_block_start") open_blocks_.insert(e.at("index").get<int>());
    if (type == "content_block_stop") open_blocks_.erase(e.at("index").get<int>());
}
std::string AnthropicStreamState::finish(const Usage& u) {
    if (!started_ || done_) return {};
    done_ = true;
    std::string out;
    auto emit = [&](const nlohmann::json& e) {
        out += "event: " + e.at("type").get<std::string>() + "\ndata: " + e.dump() + "\n\n";
    };
    for (int index : open_blocks_) emit({{"type", "content_block_stop"}, {"index", index}});
    open_blocks_.clear();
    emit({{"type", "message_delta"},
          {"delta", {{"stop_reason", "max_tokens"}, {"stop_sequence", nullptr}}},
          {"usage",
           {{"input_tokens", u.prompt - u.cached},
            {"cache_read_input_tokens", u.cached},
            {"output_tokens", u.completion}}}});
    emit({{"type", "message_stop"}});
    return out;
}
}  // namespace gw::proto
