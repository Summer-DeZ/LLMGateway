#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/noncopyable.hpp"
#include "gateway/config.hpp"
#include "net/event_loop.hpp"
#include "store/pg_client.hpp"

namespace gw::gateway {

class Metrics;

// 一条入账记录（usage-ledger.md §4.2）。token 字段全部来自上游报告；NULL = 未报告
struct UsageRow {
    std::string ledger_id;  // 网关生成的唯一 ID（16 字节随机十六进制）
    std::string client_request_id;
    std::string gateway_instance;
    std::string key_name;
    std::string billing_day;  // YYYY-MM-DD，按 started_at 与配置的 UTC 偏移
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
    std::string ingress;
    std::string model;
    std::string upstream;  // 空串 → NULL
    bool stream = false;
    std::string outcome;
    int http_status = 0;
    std::optional<std::int64_t> input_tokens;
    std::optional<std::int64_t> cached_tokens;
    std::optional<std::int64_t> computed_input_tokens;
    std::optional<std::int64_t> output_tokens;
    std::int64_t billable_tokens = 0;  // 各尝试 consumed 之和；consumed 为 null 的尝试按 0 计
    bool usage_complete = true;
    std::optional<std::int64_t> cost_micro_usd;
    bool truncated = false;
    std::optional<std::int64_t> latency_ms;
    std::optional<std::int64_t> ttft_ms;
    nlohmann::json attempts = nlohmann::json::array();

    // ledger_insert 的 $1..$24 参数（文本格式）；单测覆盖 NULL 字段、成本整数化、attempts JSON
    std::vector<std::optional<std::string>> toParams() const;
};

// epoch 毫秒 → timestamptz 文本（UTC）
std::string pgTimestamp(std::int64_t wall_ms);

// 批量入账。io 线程 submit（入队即返回）；ledger loop 每 200ms 或攒够 batch_rows 提交一个事务。
// 计费不可用（不健康）时网关 fail closed（准入返回 503）
class LedgerWriter : noncopyable {
public:
    LedgerWriter(net::EventLoop* ledger_loop, std::shared_ptr<store::PgClient> write_client,
                 const LedgerConfig& config, std::string instance_id, Metrics* metrics = nullptr);
    ~LedgerWriter();

    void start();  // ledger loop 上调用：启动批量提交定时器
    bool submit(UsageRow row);
    bool healthy() const;  // 任意线程可读（原子标志）
    std::size_t backlog() const;
    // main 线程调用：阻塞等待队列写完（最长 max_wait_seconds），然后停批量定时器
    void flushAndStop(int max_wait_seconds);

private:
    void drainOnce();
    void setBatchTimer();

    net::EventLoop* loop_;
    std::shared_ptr<store::PgClient> write_client_;
    LedgerConfig config_;
    std::string instance_id_;
    Metrics* metrics_ = nullptr;

    mutable std::mutex mutex_;
    std::deque<UsageRow> queue_;
    std::condition_variable cv_;
    std::atomic<bool> last_commit_ok_{true};
    std::size_t in_flight_ = 0;  // protected by mutex_
    std::atomic<bool> stopping_{false};
    net::TimerId batch_timer_;
};

// 预编译语句集合（写连接）
std::vector<store::PgPreparedStatement> ledgerWriteStatements();
// 读连接
std::vector<store::PgPreparedStatement> ledgerReadStatements();

// 启动时执行迁移（ledger loop 上调用；advisory lock 保证多实例只有一个执行）
void runMigrations(store::PgClient& client, std::function<void(std::expected<void, std::string>)> done);

}  // namespace gw::gateway
