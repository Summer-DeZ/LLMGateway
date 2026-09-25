#include "gateway/ledger.hpp"

#include <chrono>

#include "base/log.hpp"
#include "base/random.hpp"
#include "gateway/metrics.hpp"
#include "gw_migrations_embedded.h"

namespace gw::gateway {

namespace {

// ledger_insert：一条语句同时写明细和汇总（幂等：重复 ledger_id 走 DO NOTHING，CTE 返回空）
constexpr const char* kLedgerInsertSql = R"sql(
WITH ins AS (
    INSERT INTO usage_records (ledger_id, client_request_id, gateway_instance, key_name, billing_day, started_at,
                               finished_at, ingress, model, upstream, stream, outcome, http_status, input_tokens,
                               cached_tokens, computed_input_tokens, output_tokens, billable_tokens, usage_complete,
                               cost_micro_usd, truncated, latency_ms, ttft_ms, attempts)
    VALUES ($1, $2, $3, $4, $5, $6::timestamptz, $7::timestamptz, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17,
            $18, $19, $20, $21, $22, $23, $24::jsonb)
    ON CONFLICT (ledger_id) DO NOTHING
    RETURNING key_name, billing_day, model, input_tokens, cached_tokens, output_tokens, billable_tokens,
              cost_micro_usd, usage_complete, truncated
)
INSERT INTO usage_daily AS d
SELECT key_name, billing_day, model, 1, coalesce(input_tokens, 0), coalesce(cached_tokens, 0),
       coalesce(output_tokens, 0), billable_tokens, coalesce(cost_micro_usd, 0), (NOT usage_complete)::int,
       truncated::int
FROM ins
ON CONFLICT (key_name, day, model) DO UPDATE SET
    requests = d.requests + 1,
    input_tokens = d.input_tokens + EXCLUDED.input_tokens,
    cached_tokens = d.cached_tokens + EXCLUDED.cached_tokens,
    output_tokens = d.output_tokens + EXCLUDED.output_tokens,
    billable_tokens = d.billable_tokens + EXCLUDED.billable_tokens,
    cost_micro_usd = d.cost_micro_usd + EXCLUDED.cost_micro_usd,
    unreported_requests = d.unreported_requests + EXCLUDED.unreported_requests,
    truncated_requests = d.truncated_requests + EXCLUDED.truncated_requests
)sql";

std::optional<std::string> toStr(std::int64_t v) { return std::to_string(v); }

}  // namespace

std::string pgTimestamp(std::int64_t wall_ms) {
    std::time_t secs = static_cast<std::time_t>(wall_ms / 1000);
    int millis = static_cast<int>(wall_ms % 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d+00", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
    return buf;
}

std::vector<std::optional<std::string>> UsageRow::toParams() const {
    std::vector<std::optional<std::string>> p;
    p.reserve(24);
    p.push_back(ledger_id);
    p.push_back(client_request_id.empty() ? std::nullopt : std::optional<std::string>(client_request_id));
    p.push_back(gateway_instance);
    p.push_back(key_name);
    p.push_back(billing_day);
    p.push_back(pgTimestamp(started_at_ms));
    p.push_back(pgTimestamp(finished_at_ms));
    p.push_back(ingress);
    p.push_back(model);
    p.push_back(upstream.empty() ? std::nullopt : std::optional<std::string>(upstream));
    p.push_back(std::string(stream ? "true" : "false"));
    p.push_back(outcome);
    p.push_back(std::to_string(http_status));
    p.push_back(input_tokens ? toStr(*input_tokens) : std::nullopt);
    p.push_back(cached_tokens ? toStr(*cached_tokens) : std::nullopt);
    p.push_back(computed_input_tokens ? toStr(*computed_input_tokens) : std::nullopt);
    p.push_back(output_tokens ? toStr(*output_tokens) : std::nullopt);
    p.push_back(std::to_string(billable_tokens));
    p.push_back(std::string(usage_complete ? "true" : "false"));
    p.push_back(cost_micro_usd ? toStr(*cost_micro_usd) : std::nullopt);
    p.push_back(std::string(truncated ? "true" : "false"));
    p.push_back(latency_ms ? toStr(*latency_ms) : std::nullopt);
    p.push_back(ttft_ms ? toStr(*ttft_ms) : std::nullopt);
    p.push_back(attempts.is_null() ? std::string("[]") : attempts.dump());
    return p;
}

std::vector<store::PgPreparedStatement> ledgerWriteStatements() {
    return {
        {"ledger_insert", kLedgerInsertSql, 24},
    };
}

std::vector<store::PgPreparedStatement> ledgerReadStatements() {
    return {
        {"usage_daily_by_key",
         "SELECT day, model, requests, input_tokens, cached_tokens, output_tokens, billable_tokens, "
         "cost_micro_usd, unreported_requests, truncated_requests FROM usage_daily "
         "WHERE key_name = $1 AND day >= $2::date AND day <= $3::date ORDER BY day, model",
         3},
        {"usage_records_page",
         "SELECT finished_at, ledger_id, client_request_id, model, upstream, outcome, http_status, input_tokens, "
         "cached_tokens, computed_input_tokens, output_tokens, billable_tokens, usage_complete, cost_micro_usd, "
         "truncated, latency_ms, ttft_ms FROM usage_records "
         "WHERE key_name = $1 AND ($2::timestamptz IS NULL OR finished_at < $2::timestamptz) "
         "ORDER BY finished_at DESC, ledger_id DESC LIMIT $3",
         3},
        {"usage_day_sum",
         "SELECT coalesce(sum(billable_tokens), 0) FROM usage_records WHERE key_name = $1 AND billing_day = $2::date",
         2},
        {"usage_month_sum",
         "SELECT coalesce(sum(billable_tokens), 0) FROM usage_records WHERE key_name = $1 "
         "AND date_trunc('month', billing_day)::date = $2::date",
         2},
        {"usage_daily_grouped_by_key",
         "SELECT key_name, day, model, requests, input_tokens, cached_tokens, output_tokens, billable_tokens, "
         "cost_micro_usd, unreported_requests, truncated_requests FROM usage_daily "
         "WHERE day >= $1::date AND day <= $2::date ORDER BY key_name, day, model",
         2},
    };
}

void runMigrations(store::PgClient& client, std::function<void(std::expected<void, std::string>)> done) {
    // 1) advisory lock（多实例同时启动时只有一个执行）+ 迁移表；2) 读已应用版本；
    // 3) 未应用则逐条执行 001 并记录版本；4) 解锁。DDL 失败时不记录版本，重跑安全
    client.rawBatch({"SELECT pg_advisory_lock(727001)",
                     "CREATE TABLE IF NOT EXISTS schema_migrations (version int PRIMARY KEY, "
                     "applied_at timestamptz NOT NULL DEFAULT now())"},
                    [&client, done](std::expected<void, store::PgError> lock_result) {
                        if (!lock_result) {
                            done(std::unexpected(lock_result.error().message));
                            return;
                        }
                        auto unlock = [&client, done](bool ok, const std::string& error) {
                            client.rawBatch({"SELECT pg_advisory_unlock(727001)"},
                                            [done, ok, error](std::expected<void, store::PgError>) {
                                                if (ok) done({});
                                                else done(std::unexpected(error));
                                            });
                        };
                        client.rawQuery("SELECT coalesce(max(version), 0) FROM schema_migrations",
                                        [&client, done, unlock](std::expected<store::PgResult, store::PgError> qr) {
                                            if (!qr) {
                                                unlock(false, qr.error().message);
                                                return;
                                            }
                                            std::int64_t applied = 0;
                                            if (!qr->rows.empty() && qr->rows[0].columns[0]) {
                                                applied = std::strtoll(qr->rows[0].columns[0]->c_str(), nullptr, 10);
                                            }
                                            if (applied >= 2) {
                                                unlock(true, "");
                                                return;
                                            }
                                            // 应用 001：按分号拆分（本项目迁移不使用带分号的字符串字面量）
                                            std::vector<std::string> sqls;
                                            std::string current;
                                            const std::string migration = (applied < 1 ? std::string(kMigration001) : "") + kMigration002;
                                            sqls.push_back("BEGIN");
                                            for (char c : migration) {
                                                if (c == ';') {
                                                    if (!current.empty()) sqls.push_back(current);
                                                    current.clear();
                                                } else {
                                                    current.push_back(c);
                                                }
                                            }
                                            if (!current.empty()) sqls.push_back(current);
                                            if (applied < 1) sqls.push_back("INSERT INTO schema_migrations (version) VALUES (1)");
                                            sqls.push_back("INSERT INTO schema_migrations (version) VALUES (2)");
                                            sqls.push_back("COMMIT");
                                            client.rawBatch(
                                                std::move(sqls),
                                                [&client, unlock](std::expected<void, store::PgError> ar) {
                                                    if (!ar) {
                                                        unlock(false, ar.error().message);
                                                        return;
                                                    }
                                                    unlock(true, "");
                                                });
                                        });
                    });
}

LedgerWriter::LedgerWriter(net::EventLoop* ledger_loop, std::shared_ptr<store::PgClient> write_client,
                           const LedgerConfig& config, std::string instance_id, Metrics* metrics)
    : loop_(ledger_loop), write_client_(std::move(write_client)), config_(config),
      instance_id_(std::move(instance_id)), metrics_(metrics) {}

LedgerWriter::~LedgerWriter() {
    if (batch_timer_.valid()) loop_->cancel(batch_timer_);
}

void LedgerWriter::start() {
    setBatchTimer();
}

void LedgerWriter::setBatchTimer() {
    batch_timer_ = loop_->runAfter(config_.batch_interval, [this] { drainOnce(); });
}

bool LedgerWriter::submit(UsageRow row) {
    std::scoped_lock lock(mutex_);
    if (queue_.size() + in_flight_ >= config_.max_backlog) return false;  // 队列满：这行丢弃并计数（healthy 已变 false，网关在拒绝新请求）
    queue_.push_back(std::move(row));
    return true;
}

bool LedgerWriter::healthy() const {
    // fail closed：写连接不在线（含重连退避期间）或最近一批提交失败都不提供服务
    return write_client_->connected() && last_commit_ok_.load(std::memory_order_relaxed) &&
           backlog() < config_.max_backlog;
}

std::size_t LedgerWriter::backlog() const {
    std::scoped_lock lock(mutex_);
    return queue_.size() + in_flight_;
}

void LedgerWriter::drainOnce() {
    loop_->assertInLoopThread();
    std::vector<UsageRow> batch;
    {
        std::scoped_lock lock(mutex_);
        while (in_flight_ == 0 && !queue_.empty() && batch.size() < static_cast<std::size_t>(config_.batch_rows)) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        if (!batch.empty()) in_flight_ = batch.size();
    }
    if (!batch.empty()) {
        std::vector<store::PgCall> calls;
        calls.reserve(batch.size());
        for (auto& row : batch) {
            auto params = row.toParams();
            calls.push_back(store::PgCall{"ledger_insert", std::move(params), {}});
        }
        auto started = nowSteady();
        write_client_->pipeline(
            std::move(calls), [this, batch = std::move(batch), started](std::expected<void, store::PgError> done) {
                if (metrics_) metrics_->observeLedgerCommit(nowSteady() - started);
                if (done) {
                    last_commit_ok_.store(true, std::memory_order_relaxed);
                    SPDLOG_DEBUG("ledger committed {} rows", batch.size());
                } else {
                    // 失败：整批放回队首，重连后重试
                    last_commit_ok_.store(false, std::memory_order_relaxed);
                    if (metrics_) metrics_->observeLedgerWriteError();
                    std::scoped_lock lock(mutex_);
                    for (std::size_t i = batch.size(); i > 0; --i) queue_.push_front(std::move(batch[i - 1]));
                    SPDLOG_WARN("ledger commit failed: {} ({} rows requeued)", done.error().message, batch.size());
                }
                std::scoped_lock lock(mutex_);
                in_flight_ = 0;
                cv_.notify_all();
            });
    } else {
        std::scoped_lock lock(mutex_);
        cv_.notify_all();
    }
    if (!stopping_) setBatchTimer();
}

void LedgerWriter::flushAndStop(int max_wait_seconds) {
    // main 线程：等待队列写完（ledger loop 继续按周期 drain），超时则放弃剩余行
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(max_wait_seconds);
    while (backlog() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    stopping_ = true;
    loop_->queueInLoop([this] {
        if (batch_timer_.valid()) {
            loop_->cancel(batch_timer_);
            batch_timer_ = net::TimerId{};
        }
    });
}

}  // namespace gw::gateway
