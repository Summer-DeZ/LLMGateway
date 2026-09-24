#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "gateway/usage_meter.hpp"
#include "proto/neutral.hpp"

namespace gw::proto {

struct OpenAiRequestMeta {
    std::string model;
    bool stream = false;
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    bool client_wants_usage = false;
    bool client_requested_metering = false;
    bool n_greater_than_one = false;
};

std::expected<OpenAiRequestMeta, std::string> parseOpenAiRequest(const nlohmann::json& body);

// 透传最小改写：model、include_usage、metering 注入、extra_body 只补缺失的顶层字段
nlohmann::json rewriteOpenAiRequest(const nlohmann::json& original, const std::string& upstream_model,
                                    const std::string& metering, const nlohmann::json& extra_body, bool stream);

// 非流式响应的 usage 提取（结束时的精确值）
struct OpenAiUsageView {
    std::int64_t prompt = 0;
    std::int64_t completion = 0;
    std::optional<std::int64_t> cached;
    bool present = false;
};
OpenAiUsageView extractOpenAiUsage(const nlohmann::json& body);

class OpenAiStreamObserver {
public:
    struct Result {
        enum class Action { drop, forward, done, error };
        Action action = Action::drop;
        std::string data;
        bool first_token = false;
        std::string error_message;
    };

    OpenAiStreamObserver(gateway::UsageMeter* meter, bool client_wants_usage, bool client_requested_metering);

    Result process(std::string_view data);

    bool sawFirstToken() const { return saw_first_token_; }
    bool done() const { return done_; }
    const std::string& finishReason() const { return finish_reason_; }
    bool usageSeen() const { return usage_seen_; }
    bool sawProgress() const { return saw_progress_; }

private:
    gateway::UsageMeter* meter_;
    bool client_wants_usage_;
    bool client_requested_metering_;
    bool saw_first_token_ = false;
    bool done_ = false;
    bool usage_seen_ = false;
    bool saw_progress_ = false;
    std::string finish_reason_;
};

}  // namespace gw::proto
