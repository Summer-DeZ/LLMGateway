#include "gateway/usage_meter.hpp"
#include <algorithm>

namespace gw::gateway {

std::optional<std::int64_t> AttemptUsage::consumed() const {
    if (!cached || !prompt_computed || !completion) return std::nullopt;
    return *cached + *prompt_computed + *completion;
}

std::optional<std::int64_t> AttemptUsage::projected() const {
    if (!prompt || !completion) return std::nullopt;
    return *prompt + *completion;
}

UsageMeter::UsageMeter(std::string metering) : metering_(std::move(metering)) {}

void UsageMeter::onPromptProgress(std::int64_t total, std::int64_t cache, std::int64_t processed) {
    usage_.prompt = total;
    usage_.cached = cache;
    // llama.cpp reports the total populated prompt position, including cache hits.
    usage_.prompt_computed = std::max<std::int64_t>(0, processed - cache);
}

void UsageMeter::onTimings(std::int64_t cache_n, std::int64_t prompt_n, std::int64_t predicted_n) {
    usage_.cached = cache_n;
    usage_.prompt_computed = prompt_n;
    usage_.prompt = cache_n + prompt_n;
    usage_.completion = predicted_n;
}

void UsageMeter::onChunkUsage(std::int64_t prompt, std::int64_t completion, std::optional<std::int64_t> cached) {
    usage_.prompt = prompt;
    usage_.completion = completion;
    if (cached) usage_.cached = cached;
    if (!usage_.prompt_computed && usage_.prompt && usage_.cached) {
        usage_.prompt_computed = *usage_.prompt - *usage_.cached;
    }
}

void UsageMeter::onAnthropicMessageStart(std::int64_t input, std::int64_t cache_read,
                                         std::int64_t cache_creation) {
    usage_.cached = cache_read;
    usage_.prompt = input + cache_read + cache_creation;
    usage_.prompt_computed = input + cache_creation;
    output_seen_ = true;  // message_start 说明预填充已完成
}

void UsageMeter::onAnthropicMessageDelta(std::int64_t output) {
    usage_.completion = output;
}

void UsageMeter::markOutputSeen() {
    output_seen_ = true;
    if (!usage_.prompt_computed && usage_.prompt && usage_.cached) {
        usage_.prompt_computed = *usage_.prompt - *usage_.cached;
    }
}

void UsageMeter::markFinalUsage() {
    final_usage_seen_ = true;
}

void UsageMeter::markRejected() {
    rejected_ = true;
}

AttemptUsage UsageMeter::finish(bool stream_completed) {
    if (usage_.source != AttemptUsage::Source::none) return usage_;
    if (rejected_) {
        usage_.prompt = 0;
        usage_.cached = 0;
        usage_.prompt_computed = 0;
        usage_.completion = 0;
        usage_.source = AttemptUsage::Source::rejected;
        return usage_;
    }
    if (final_usage_seen_) {
        usage_.source = AttemptUsage::Source::upstream_final;
    } else if (stream_completed) {
        // 上游正常结束但没给 usage：能拿到的字段保留，标 partial
        usage_.source = usage_.prompt || usage_.completion ? AttemptUsage::Source::upstream_partial
                                                           : AttemptUsage::Source::unreported;
    } else if (usage_.prompt || usage_.completion || usage_.cached || usage_.prompt_computed) {
        usage_.source = AttemptUsage::Source::upstream_partial;
    } else {
        usage_.source = AttemptUsage::Source::unreported;
    }
    return usage_;
}

}  // namespace gw::gateway
