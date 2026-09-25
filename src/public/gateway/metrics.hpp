#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"

namespace gw::gateway {

class Config;
struct RequestRecord;

// Prometheus 指标。所有标签组合在启动时按配置注册，运行期只做原子加（policies.md §3.5）。
class Metrics : noncopyable {
public:
    explicit Metrics(const Config& config);
    ~Metrics();

    // 请求结束的统一入口：requests_total、tokens_total、cost、直方图、wasted
    void observeRequest(const RequestRecord& record);
    void observeAttempt(const std::string& upstream, std::string_view result);
    void observeUpstreamTtfb(const std::string& upstream, Duration ttfb);
    void observeUnreported(const std::string& upstream, std::string_view field);
    void observeCache(bool hit);
    void observeCacheSavedCost(const std::string& model, std::int64_t micro_usd);
    void setCacheGauges(std::uint64_t bytes, std::uint64_t entries);
    void observeRatelimitRejection(std::string_view scope, std::string_view dimension);
    void observeQuotaRejection(const std::string& user, std::string_view period = "day");
    void observeQuotaTruncation(const std::string& user);
    void observeRedisCommand(std::string_view command);
    void observeRedisError(std::string_view kind);
    void observeRedisLatency(Duration latency);
    void observeLedgerCommit(Duration latency);
    void observeLedgerWriteError();
    void setLedgerBacklog(std::uint64_t backlog);
    void observeCancellation(const std::string& model, std::string_view stage);
    void setActiveStreams(std::int64_t value);
    void addActiveStreams(std::int64_t delta);
    void addBackpressureActive(std::int64_t delta);
    void observeBackpressurePause();
    void setCircuitState(const std::string& upstream, int state);  // 0 closed / 1 open / 2 half_open
    void setUpstreamIdle(const std::string& upstream, std::uint64_t idle);

    // Prometheus 文本格式 0.0.4（/metrics）
    std::string renderPrometheus() const;
    // /admin/stats 的 JSON 汇总（p50/p95 由直方图估算）
    std::string renderStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gw::gateway
