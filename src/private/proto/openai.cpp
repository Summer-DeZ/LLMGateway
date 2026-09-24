#include "proto/openai.hpp"

#include <algorithm>

namespace gw::proto {

std::expected<OpenAiRequestMeta, std::string> parseOpenAiRequest(const nlohmann::json& body) {
    OpenAiRequestMeta meta;
    if (!body.is_object()) return std::unexpected("request body must be a JSON object");
    if (!body.contains("model") || !body.at("model").is_string()) {
        return std::unexpected("missing or invalid 'model'");
    }
    meta.model = body.at("model").get<std::string>();
    if (meta.model.empty()) return std::unexpected("model must not be empty");
    if (!body.contains("messages") || !body.at("messages").is_array() || body.at("messages").empty()) {
        return std::unexpected("missing or empty 'messages'");
    }
    if (body.contains("stream")) {
        if (!body.at("stream").is_boolean()) return std::unexpected("'stream' must be a boolean");
        meta.stream = body.at("stream").get<bool>();
    }
    if (body.contains("max_tokens") && !body.at("max_tokens").is_null()) {
        meta.max_tokens = body.at("max_tokens").get<int>();
    } else if (body.contains("max_completion_tokens") && !body.at("max_completion_tokens").is_null()) {
        meta.max_tokens = body.at("max_completion_tokens").get<int>();
    }
    if (body.contains("temperature") && !body.at("temperature").is_null()) {
        meta.temperature = body.at("temperature").get<double>();
    }
    if (body.contains("n") && body.at("n").is_number_integer()) {
        meta.n_greater_than_one = body.at("n").get<int>() > 1;
    }
    if (body.contains("stream_options") && body.at("stream_options").is_object()) {
        const auto& options = body.at("stream_options");
        if (options.contains("include_usage") && options.at("include_usage").is_boolean()) {
            meta.client_wants_usage = options.at("include_usage").get<bool>();
        }
    }
    bool timings = body.value("timings_per_token", false);
    bool progress = body.value("return_progress", false);
    bool continuous = false;
    if (body.contains("stream_options") && body.at("stream_options").is_object()) {
        continuous = body.at("stream_options").value("continuous_usage_stats", false);
    }
    meta.client_requested_metering = timings || progress || continuous;
    return meta;
}

nlohmann::json rewriteOpenAiRequest(const nlohmann::json& original, const std::string& upstream_model,
                                    const std::string& metering, const nlohmann::json& extra_body, bool stream) {
    nlohmann::json out = original;
    out["model"] = upstream_model;
    if (stream) {
        out["stream"] = true;
        if (!out.contains("stream_options") || !out.at("stream_options").is_object()) {
            out["stream_options"] = nlohmann::json::object();
        }
        out["stream_options"]["include_usage"] = true;
        if (metering == "llamacpp") {
            out["timings_per_token"] = true;
            out["return_progress"] = true;
        } else if (metering == "vllm") {
            out["stream_options"]["continuous_usage_stats"] = true;
        }
    }
    if (extra_body.is_object()) {
        for (const auto& [key, value] : extra_body.items()) {
            if (!out.contains(key)) out[key] = value;
        }
    }
    return out;
}

OpenAiUsageView extractOpenAiUsage(const nlohmann::json& body) {
    OpenAiUsageView view;
    if (!body.is_object() || !body.contains("usage") || !body.at("usage").is_object()) return view;
    const auto& usage = body.at("usage");
    view.present = true;
    view.prompt = usage.value("prompt_tokens", std::int64_t{0});
    view.completion = usage.value("completion_tokens", std::int64_t{0});
    if (usage.contains("prompt_tokens_details") && usage.at("prompt_tokens_details").is_object()) {
        const auto& details = usage.at("prompt_tokens_details");
        if (details.contains("cached_tokens") && details.at("cached_tokens").is_number_integer()) {
            view.cached = details.at("cached_tokens").get<std::int64_t>();
        }
    }
    return view;
}

OpenAiStreamObserver::OpenAiStreamObserver(gateway::UsageMeter* meter, bool client_wants_usage,
                                           bool client_requested_metering)
    : meter_(meter), client_wants_usage_(client_wants_usage), client_requested_metering_(client_requested_metering) {}

OpenAiStreamObserver::Result OpenAiStreamObserver::process(std::string_view data) {
    Result result;
    if (data == "[DONE]") {
        done_ = true;
        if (usage_seen_ && meter_) meter_->markFinalUsage();
        result.action = Result::Action::done;
        result.data = "[DONE]";
        return result;
    }

    nlohmann::json chunk;
    try {
        chunk = nlohmann::json::parse(data);
    } catch (...) {
        // 解析失败：原样转发，不更新计量
        result.action = Result::Action::forward;
        result.data = std::string(data);
        return result;
    }

    if (chunk.contains("error")) {
        std::string message = "upstream error";
        if (chunk.at("error").is_object() && chunk.at("error").contains("message")) {
            message = chunk.at("error").at("message").get<std::string>();
        } else if (chunk.at("error").is_string()) {
            message = chunk.at("error").get<std::string>();
        }
        result.action = Result::Action::error;
        result.error_message = message;
        return result;
    }

    bool has_usage = chunk.contains("usage") && chunk.at("usage").is_object();
    if (has_usage) {
        auto view = extractOpenAiUsage(chunk);
        if (view.present && meter_) {
            meter_->onChunkUsage(view.prompt, view.completion, view.cached);
            usage_seen_ = true;
        }
    }
    if (chunk.contains("timings") && chunk.at("timings").is_object() && meter_) {
        const auto& timings = chunk.at("timings");
        meter_->onTimings(timings.value("cache_n", std::int64_t{0}), timings.value("prompt_n", std::int64_t{0}),
                          timings.value("predicted_n", std::int64_t{0}));
    }
    if (chunk.contains("prompt_progress") && chunk.at("prompt_progress").is_object()) {
        saw_progress_ = true;
    }
    if (chunk.contains("prompt_progress") && chunk.at("prompt_progress").is_object() && meter_) {
        const auto& progress = chunk.at("prompt_progress");
        meter_->onPromptProgress(progress.value("total", std::int64_t{0}), progress.value("cache", std::int64_t{0}),
                                 progress.value("processed", std::int64_t{0}));
    }

    bool delta_content = false;
    bool has_choices = chunk.contains("choices") && chunk.at("choices").is_array() && !chunk.at("choices").empty();
    std::string finish_reason;
    if (has_choices) {
        const auto& choice = chunk.at("choices").at(0);
        if (choice.contains("delta") && choice.at("delta").is_object()) {
            const auto& delta = choice.at("delta");
            if (delta.contains("content") && delta.at("content").is_string() && !delta.at("content").get<std::string>().empty()) {
                delta_content = true;
            }
            if (delta.contains("reasoning_content") && delta.at("reasoning_content").is_string() &&
                !delta.at("reasoning_content").get<std::string>().empty()) {
                delta_content = true;
            }
            if (delta.contains("reasoning") && delta.at("reasoning").is_string() &&
                !delta.at("reasoning").get<std::string>().empty()) {
                delta_content = true;
            }
            if (delta.contains("tool_calls") && delta.at("tool_calls").is_array() && !delta.at("tool_calls").empty()) {
                delta_content = true;
            }
        }
        if (choice.contains("finish_reason") && choice.at("finish_reason").is_string()) {
            finish_reason = choice.at("finish_reason").get<std::string>();
            finish_reason_ = finish_reason;
        }
    }

    if (delta_content && !saw_first_token_) {
        saw_first_token_ = true;
        result.first_token = true;
        if (meter_) meter_->markOutputSeen();
    }

    // 丢弃 choices 为空且只含 usage 的那一块（客户端没要 usage 时）
    if (!has_choices && has_usage && !client_wants_usage_) {
        result.action = Result::Action::drop;
        return result;
    }
    // 只含进度、没有 delta 内容的 chunk 整个丢弃
    if (!delta_content && finish_reason.empty() && !(has_usage && client_wants_usage_)) {
        result.action = Result::Action::drop;
        return result;
    }

    if (!client_requested_metering_) {
        chunk.erase("timings");
        chunk.erase("prompt_progress");
    }
    if (!client_wants_usage_) {
        chunk.erase("usage");
    }
    result.action = Result::Action::forward;
    result.data = chunk.dump();
    return result;
}

}  // namespace gw::proto
