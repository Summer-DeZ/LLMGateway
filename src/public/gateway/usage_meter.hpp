#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace gw::gateway {

// 每次尝试一个实例；所有字段都是累计值，只覆盖不相加
struct AttemptUsage {
    std::optional<std::int64_t> prompt;           // prompt 总 token（含缓存命中部分）
    std::optional<std::int64_t> cached;           // 命中缓存、未重新计算的部分
    std::optional<std::int64_t> prompt_computed;  // 上游实际做了预填充的 token 数
    std::optional<std::int64_t> completion;       // 上游已生成的 token 数（累计）

    enum class Source { none, rejected, upstream_final, upstream_partial, unreported };
    Source source = Source::none;

    // 消耗量 = cached + prompt_computed + completion（全项目唯一口径）
    std::optional<std::int64_t> consumed() const;
    // 预判量 = prompt + completion（prompt 取总数）；只用于配额检查
    std::optional<std::int64_t> projected() const;
};

class UsageMeter {
public:
    explicit UsageMeter(std::string metering);

    void onPromptProgress(std::int64_t total, std::int64_t cache, std::int64_t processed);
    void onTimings(std::int64_t cache_n, std::int64_t prompt_n, std::int64_t predicted_n);
    // vLLM 逐块 usage / OpenAI 末块 usage（累计值）
    void onChunkUsage(std::int64_t prompt, std::int64_t completion, std::optional<std::int64_t> cached);
    void onAnthropicMessageStart(std::int64_t input, std::int64_t cache_read, std::int64_t cache_creation);
    void onAnthropicMessageDelta(std::int64_t output);
    // 观察到任何输出事件（文本/思考/工具调用增量）或 Anthropic message_start
    void markOutputSeen();
    // 上游报告了结束时的 usage（OpenAI 末块 / Anthropic message_delta）
    void markFinalUsage();
    void markRejected();

    // 结束本次尝试；stream_completed 表示上游正常结束标记已到
    AttemptUsage finish(bool stream_completed);

    const AttemptUsage& value() const { return usage_; }
    const std::string& metering() const { return metering_; }
    bool finalUsageSeen() const { return final_usage_seen_; }

private:
    std::string metering_;
    AttemptUsage usage_;
    bool output_seen_ = false;
    bool final_usage_seen_ = false;
    bool rejected_ = false;
};

}  // namespace gw::gateway
