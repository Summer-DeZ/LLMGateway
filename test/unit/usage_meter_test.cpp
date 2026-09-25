#include "gateway/usage_meter.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "proto/openai.hpp"

using gw::gateway::AttemptUsage;
using gw::gateway::UsageMeter;

namespace {

std::vector<std::string> loadDataLines(const std::string& name) {
    std::ifstream in(std::string(GW_TEST_DATA_DIR) + "/" + name);
    EXPECT_TRUE(in.good()) << "cannot open test data " << name;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("data: ")) lines.push_back(line.substr(6));
    }
    return lines;
}

}  // namespace

// 用录制的真实 Qwen3.6 流逐事件喂入，每一步的累计值与样本里的字段一致，结束时等于末块 usage
TEST(UsageMeterTest, MatchesRecordedQwenStream) {
    auto lines = loadDataLines("openai_metering.sse");
    ASSERT_GT(lines.size(), 100u);

    UsageMeter meter("llamacpp");
    gw::proto::OpenAiStreamObserver observer(&meter, true, true);

    std::int64_t expect_prompt = 0, expect_cached = 0, expect_computed = 0, expect_completion = 0;
    bool final_seen = false;
    for (const auto& data : lines) {
        if (data == "[DONE]") {
            auto result = observer.process(data);
            EXPECT_EQ(result.action, gw::proto::OpenAiStreamObserver::Result::Action::done);
            continue;
        }
        auto chunk = nlohmann::json::parse(data);
        if (chunk.contains("timings")) {
            const auto& t = chunk.at("timings");
            expect_cached = t.value("cache_n", std::int64_t{0});
            expect_computed = t.value("prompt_n", std::int64_t{0});
            expect_prompt = expect_cached + expect_computed;
            expect_completion = t.value("predicted_n", std::int64_t{0});
        }
        if (chunk.contains("prompt_progress")) {
            const auto& p = chunk.at("prompt_progress");
            expect_prompt = p.value("total", std::int64_t{0});
            expect_cached = p.value("cache", std::int64_t{0});
            expect_computed = p.value("processed", std::int64_t{0});
        }
        if (chunk.contains("usage")) {
            const auto& u = chunk.at("usage");
            expect_prompt = u.value("prompt_tokens", std::int64_t{0});
            expect_completion = u.value("completion_tokens", std::int64_t{0});
            if (u.contains("prompt_tokens_details")) {
                expect_cached = u.at("prompt_tokens_details").value("cached_tokens", std::int64_t{0});
            }
            final_seen = true;
        }
        observer.process(data);

        const auto& usage = meter.value();
        ASSERT_TRUE(usage.prompt.has_value()) << data;
        ASSERT_TRUE(usage.completion.has_value()) << data;
        EXPECT_EQ(*usage.prompt, expect_prompt) << data;
        EXPECT_EQ(*usage.cached, expect_cached) << data;
        EXPECT_EQ(*usage.prompt_computed, expect_computed) << data;
        EXPECT_EQ(*usage.completion, expect_completion) << data;
    }
    ASSERT_TRUE(final_seen);
    auto usage = meter.finish(true);
    EXPECT_EQ(usage.source, AttemptUsage::Source::upstream_final);
    EXPECT_EQ(usage.prompt, expect_prompt);
    EXPECT_EQ(usage.cached, expect_cached);
    EXPECT_EQ(usage.completion, expect_completion);
    EXPECT_EQ(usage.consumed(), expect_cached + expect_computed + expect_completion);
}

// 在任意事件处截断，meter 的值等于截断前最后一个 timings（或 prompt_progress）
TEST(UsageMeterTest, TruncatedMatchesLastTimings) {
    auto lines = loadDataLines("openai_metering.sse");
    for (std::size_t cut : {std::size_t{1}, lines.size() / 3, lines.size() / 2, lines.size() - 2}) {
        UsageMeter meter("llamacpp");
        gw::proto::OpenAiStreamObserver observer(&meter, true, true);
        std::int64_t expect_prompt = 0, expect_cached = 0, expect_computed = 0, expect_completion = 0;
        for (std::size_t i = 0; i < cut && i < lines.size(); ++i) {
            if (lines[i] == "[DONE]") break;
            auto chunk = nlohmann::json::parse(lines[i]);
            if (chunk.contains("timings")) {
                const auto& t = chunk.at("timings");
                expect_cached = t.value("cache_n", std::int64_t{0});
                expect_computed = t.value("prompt_n", std::int64_t{0});
                expect_prompt = expect_cached + expect_computed;
                expect_completion = t.value("predicted_n", std::int64_t{0});
            }
            if (chunk.contains("prompt_progress")) {
                const auto& p = chunk.at("prompt_progress");
                expect_prompt = p.value("total", std::int64_t{0});
                expect_cached = p.value("cache", std::int64_t{0});
                expect_computed = p.value("processed", std::int64_t{0});
            }
            observer.process(lines[i]);
        }
        auto usage = meter.finish(false);
        EXPECT_EQ(usage.source, AttemptUsage::Source::upstream_partial) << "cut=" << cut;
        EXPECT_EQ(usage.prompt, expect_prompt) << "cut=" << cut;
        EXPECT_EQ(usage.completion, expect_completion) << "cut=" << cut;
        EXPECT_EQ(usage.prompt_computed, expect_computed) << "cut=" << cut;
    }
}

TEST(UsageMeterTest, RejectedIsAllZero) {
    UsageMeter meter("llamacpp");
    meter.markRejected();
    auto usage = meter.finish(false);
    EXPECT_EQ(usage.source, AttemptUsage::Source::rejected);
    EXPECT_EQ(usage.prompt, 0);
    EXPECT_EQ(usage.cached, 0);
    EXPECT_EQ(usage.prompt_computed, 0);
    EXPECT_EQ(usage.completion, 0);
}

TEST(UsageMeterTest, UnreportedWhenNothingSeen) {
    UsageMeter meter("final");
    auto usage = meter.finish(false);
    EXPECT_EQ(usage.source, AttemptUsage::Source::unreported);
    EXPECT_FALSE(usage.completion.has_value());
}

TEST(OpenAiStreamObserverTest, DropsProgressOnlyAndRewritesMeteringFields) {
    UsageMeter meter("llamacpp");
    gw::proto::OpenAiStreamObserver observer(&meter, false, false);

    // 只含进度的块被丢弃
    std::string progress =
        R"({"choices":[{"delta":{"role":"assistant","content":null},"finish_reason":null,"index":0}],)"
        R"("timings":{"cache_n":0,"prompt_n":0,"predicted_n":0},"prompt_progress":{"total":5,"processed":0,"cache":0}})";
    auto r1 = observer.process(progress);
    EXPECT_EQ(r1.action, gw::proto::OpenAiStreamObserver::Result::Action::drop);
    EXPECT_TRUE(observer.sawProgress());
    EXPECT_EQ(meter.value().prompt, 5);

    // 内容块：删掉 timings/prompt_progress
    std::string content =
        R"({"choices":[{"delta":{"content":"hi"},"finish_reason":null,"index":0}],)"
        R"("timings":{"cache_n":0,"prompt_n":5,"predicted_n":1}})";
    auto r2 = observer.process(content);
    ASSERT_EQ(r2.action, gw::proto::OpenAiStreamObserver::Result::Action::forward);
    EXPECT_TRUE(r2.first_token);
    EXPECT_EQ(r2.data.find("timings"), std::string::npos);
    EXPECT_NE(r2.data.find("\"content\":\"hi\""), std::string::npos);

    // usage-only 块：客户端没要 usage 时丢弃
    std::string usage_only = R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":1,"total_tokens":6}})";
    auto r3 = observer.process(usage_only);
    EXPECT_EQ(r3.action, gw::proto::OpenAiStreamObserver::Result::Action::drop);

    auto r4 = observer.process("[DONE]");
    EXPECT_EQ(r4.action, gw::proto::OpenAiStreamObserver::Result::Action::done);
    EXPECT_TRUE(observer.done());
}

TEST(OpenAiStreamObserverTest, KeepsUsageAndMeteringWhenRequested) {
    UsageMeter meter("llamacpp");
    gw::proto::OpenAiStreamObserver observer(&meter, true, true);
    std::string usage_only = R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":1,"total_tokens":6}})";
    auto r = observer.process(usage_only);
    EXPECT_EQ(r.action, gw::proto::OpenAiStreamObserver::Result::Action::forward);
    EXPECT_NE(r.data.find("usage"), std::string::npos);

    std::string content =
        R"({"choices":[{"delta":{"content":"x"},"finish_reason":null,"index":0}],"timings":{"cache_n":0,"prompt_n":5,"predicted_n":1}})";
    auto r2 = observer.process(content);
    EXPECT_NE(r2.data.find("timings"), std::string::npos);
}

TEST(OpenAiStreamObserverTest, ErrorChunk) {
    UsageMeter meter("final");
    gw::proto::OpenAiStreamObserver observer(&meter, true, true);
    auto r = observer.process(R"({"error":{"message":"boom","type":"server_error"}})");
    EXPECT_EQ(r.action, gw::proto::OpenAiStreamObserver::Result::Action::error);
    EXPECT_EQ(r.error_message, "boom");
}

TEST(UsageMeterTest, CachedPromptProgressDoesNotCountCacheTwice) {
    gw::gateway::UsageMeter meter("llamacpp");
    meter.onTimings(80, 0, 0);
    meter.onPromptProgress(100, 80, 85);
    EXPECT_EQ(meter.value().prompt_computed, 5);
    EXPECT_EQ(meter.value().consumed(), 85);
    meter.onPromptProgress(100, 80, 100);
    EXPECT_EQ(meter.value().prompt_computed, 20);
    EXPECT_EQ(meter.value().consumed(), 100);
}
