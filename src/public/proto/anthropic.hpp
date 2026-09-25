#pragma once
#include <set>

#include "proto/openai.hpp"

namespace gw::proto {
std::expected<OpenAiRequestMeta, std::string> parseAnthropicRequest(const nlohmann::json& body);
OpenAiUsageView extractAnthropicUsage(const nlohmann::json& body);
// Only observe upstream-reported counters, never derive token counts from text.
void observeAnthropicUsage(const nlohmann::json& event, gateway::UsageMeter& meter);
// Tracks exactly the blocks sent to the client on a passthrough stream.
class AnthropicStreamState {
   public:
    void observe(const nlohmann::json& event);
    std::string finish(const Usage& usage);

   private:
    bool started_ = false;
    bool done_ = false;
    std::set<int> open_blocks_;
};
}  // namespace gw::proto
