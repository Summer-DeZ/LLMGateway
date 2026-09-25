#include "gateway/usage_api.hpp"

#include <cmath>
#include <ctime>
#include <optional>

#include <nlohmann/json.hpp>

#include "base/log.hpp"
#include "base/timestamp.hpp"
#include "gateway/ledger.hpp"
#include "http/http_server.hpp"
#include "http/response_writer.hpp"
#include "proto/errors.hpp"

namespace gw::gateway {

namespace {

std::optional<std::string> queryParam(const std::string& query, std::string_view name) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        std::string_view item = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        std::size_t eq = item.find('=');
        if (eq != std::string_view::npos && item.substr(0, eq) == name) return std::string(item.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::nullopt;
}

std::optional<std::int64_t> parseNum(const std::optional<std::string>& s) {
    if (!s) return std::nullopt;
    try {
        return static_cast<std::int64_t>(std::llround(std::stod(*s)));
    } catch (...) {
        return std::nullopt;
    }
}

// usage_daily_by_key / usage_daily_grouped_by_key 的行（11 列含 key，10 列不含）
nlohmann::json rowToDailyJson(const store::PgRow& row, bool with_key) {
    nlohmann::json j;
    std::size_t base = 0;
    if (with_key) {
        j["key"] = row.columns[0] ? *row.columns[0] : "";
        base = 1;
    }
    j["day"] = row.columns[base] ? *row.columns[base] : "";
    j["model"] = row.columns[base + 1] ? *row.columns[base + 1] : "";
    auto num = [&row, base](std::size_t i) { return parseNum(row.columns[base + i]).value_or(0); };
    j["requests"] = num(2);
    j["input_tokens"] = num(3);
    j["cached_tokens"] = num(4);
    j["output_tokens"] = num(5);
    j["billable_tokens"] = num(6);
    j["cost_usd"] = microUsdToUsd(num(7));
    j["unreported_requests"] = num(8);
    j["truncated_requests"] = num(9);
    return j;
}

nlohmann::json dailyTotals(const nlohmann::json& days) {
    nlohmann::json total;
    std::int64_t requests = 0, input = 0, cached = 0, output = 0, billable = 0, unreported = 0, truncated = 0;
    double cost = 0;
    for (const auto& d : days) {
        requests += d.value("requests", 0ll);
        input += d.value("input_tokens", 0ll);
        cached += d.value("cached_tokens", 0ll);
        output += d.value("output_tokens", 0ll);
        billable += d.value("billable_tokens", 0ll);
        unreported += d.value("unreported_requests", 0ll);
        truncated += d.value("truncated_requests", 0ll);
        cost += d.value("cost_usd", 0.0);
    }
    total["requests"] = requests;
    total["input_tokens"] = input;
    total["cached_tokens"] = cached;
    total["output_tokens"] = output;
    total["billable_tokens"] = billable;
    total["cost_usd"] = cost;
    total["unreported_requests"] = unreported;
    total["truncated_requests"] = truncated;
    return total;
}

// usage_records_page 的 17 列
nlohmann::json rowToRecordJson(const store::PgRow& row) {
    nlohmann::json j;
    auto col = [&row](std::size_t i) -> std::optional<std::string> {
        if (i >= row.columns.size()) return std::nullopt;
        return row.columns[i] ? std::optional<std::string>(*row.columns[i]) : std::nullopt;
    };
    auto num = [&col](std::size_t i) { return parseNum(col(i)); };
    if (auto t = col(0)) j["finished_at"] = *t;
    if (auto v = col(1)) j["ledger_id"] = *v;
    if (auto v = col(2)) j["client_request_id"] = *v;
    j["model"] = col(3).value_or("");
    j["upstream"] = col(4).value_or("");
    j["outcome"] = col(5).value_or("");
    j["http_status"] = num(6).value_or(0);
    if (auto v = num(7)) j["input_tokens"] = *v;
    if (auto v = num(8)) j["cached_tokens"] = *v;
    if (auto v = num(9)) j["computed_input_tokens"] = *v;
    if (auto v = num(10)) j["output_tokens"] = *v;
    j["billable_tokens"] = num(11).value_or(0);
    j["usage_complete"] = col(12).value_or("f") == "t";
    if (auto v = num(13)) j["cost_usd"] = microUsdToUsd(*v);
    j["truncated"] = col(14).value_or("f") == "t";
    if (auto v = num(15)) j["latency_ms"] = *v;
    if (auto v = num(16)) j["ttft_ms"] = *v;
    return j;
}

// 下个重置时刻（本地零点或下月初）的 ISO 文本
std::string nextResetIso(std::int64_t used_day, std::int64_t used_month, const UserConfig& user,
                         const QuotaRuntimeConfig& qc, std::int64_t now_ms) {
    bool month_hit = user.quota && user.quota->monthly_tokens > 0 && used_month >= user.quota->monthly_tokens;
    bool day_hit = user.quota && user.quota->daily_tokens > 0 && used_day >= user.quota->daily_tokens;
    int64_t reset_ms = now_ms;
    if (month_hit && !day_hit) {
        std::time_t secs = static_cast<std::time_t>(now_ms / 1000 + static_cast<int64_t>(qc.utc_offset_minutes) * 60);
        std::tm tm{};
        gmtime_r(&secs, &tm);
        tm.tm_mon += 1;
        tm.tm_mday = 1;
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        time_t next = timegm(&tm);
        reset_ms = (static_cast<int64_t>(next) - qc.utc_offset_minutes * 60) * 1000;
    } else {
        int64_t local_secs = now_ms / 1000 + static_cast<int64_t>(qc.utc_offset_minutes) * 60;
        local_secs = local_secs - local_secs % 86400 + 86400;
        reset_ms = (local_secs - qc.utc_offset_minutes * 60) * 1000;
    }
    return formatWallMs(reset_ms);
}

}  // namespace

UsageApi::UsageApi(GatewayContext& context, http::HttpServer& server, net::EventLoop* ledger_loop,
                   std::shared_ptr<store::PgClient> read_client, QuotaManager* quota)
    : context_(context), server_(server), ledger_loop_(ledger_loop), read_client_(std::move(read_client)),
      quota_(quota) {}

void UsageApi::writeJson(const net::TcpConnectionPtr& conn, int status, const std::string& body) {
    bool keep = conn && conn->connected();
    http::ResponseWriter writer(conn, "", keep);
    writer.writeFull(status, {}, "application/json", body);
    server_.finishRequest(conn);
}

store::RedisClient* UsageApi::redisOf(const net::TcpConnectionPtr& conn) const {
    if (context_.quota_manager == nullptr) return nullptr;
    auto* services = std::any_cast<std::shared_ptr<IoLoopServices>>(&conn->getLoop()->context());
    return services && *services ? (*services)->redis.get() : nullptr;
}

void UsageApi::pgQuery(const net::TcpConnectionPtr& conn, const std::string& stmt, store::PgParams params,
                       std::function<void(std::expected<store::PgResult, store::PgError>)> on_done) {
    auto* read_client = read_client_.get();
    net::EventLoop* caller_loop = conn->getLoop();
    ledger_loop_->queueInLoop(
        [read_client, stmt, params = std::move(params), caller_loop, on_done = std::move(on_done)]() mutable {
            read_client->execute(stmt, params,
                                 [caller_loop, on_done = std::move(on_done)](
                                     std::expected<store::PgResult, store::PgError> r) mutable {
                                     caller_loop->queueInLoop(
                                         [on_done = std::move(on_done), r]() mutable { on_done(std::move(r)); });
                                 });
        });
}

void UsageApi::handleUsage(const http::HttpRequest& request, const net::TcpConnectionPtr& conn,
                           std::shared_ptr<const UserConfig> user) {
    if (context_.ledger_writer == nullptr) {
        writeJson(conn, 503,
                  proto::errorBody(proto::Protocol::openai, "usage ledger not configured", "service_unavailable"));
        return;
    }
    const auto& qc = context_.config->quota;
    int64_t now = wallMillis();
    std::string start = queryParam(request.query, "start").value_or(quotaBillingDay(now, qc.utc_offset_minutes));
    std::string end = queryParam(request.query, "end").value_or(start);

    auto weak_conn = std::weak_ptr<net::TcpConnection>(conn);
    pgQuery(conn, "usage_daily_by_key", {user->name, start, end},
            [this, weak_conn, user, qc](std::expected<store::PgResult, store::PgError> r) {
                auto conn = weak_conn.lock();
                if (!conn) return;
                if (!r) {
                    writeJson(conn, 503, proto::errorBody(proto::Protocol::openai,
                                                          "usage query failed: " + r.error().message,
                                                          "service_unavailable"));
                    return;
                }
                nlohmann::json out;
                out["key"] = user->name;
                nlohmann::json days = nlohmann::json::array();
                for (const auto& row : r->rows) days.push_back(rowToDailyJson(row, false));
                out["days"] = days;
                out["total"] = dailyTotals(days);

                auto* redis = redisOf(conn);
                if (!user->quota || redis == nullptr || quota_ == nullptr) {
                    writeJson(conn, 200, out.dump());
                    return;
                }
                // 配额实时状态
                int64_t day = quotaDayOf(wallMillis(), qc.utc_offset_minutes);
                int64_t month = quotaMonthOf(wallMillis(), qc.utc_offset_minutes);
                quota_->get(*redis, user->name, day, month,
                            [this, weak_conn, user, out = std::move(out),
                             qc](std::expected<QuotaUsage, store::RedisError> rq) mutable {
                                auto conn = weak_conn.lock();
                                if (!conn) return;
                                if (rq) {
                                    nlohmann::json quota;
                                    quota["daily_tokens"] = user->quota->daily_tokens;
                                    quota["monthly_tokens"] = user->quota->monthly_tokens;
                                    quota["daily_used"] = rq->used_day;
                                    quota["monthly_used"] = rq->used_month;
                                    quota["daily_remaining"] = user->quota->daily_tokens > 0
                                                                   ? user->quota->daily_tokens - rq->used_day
                                                                   : -1;
                                    quota["monthly_remaining"] = user->quota->monthly_tokens > 0
                                                                     ? user->quota->monthly_tokens - rq->used_month
                                                                     : -1;
                                    quota["resets_at"] =
                                        nextResetIso(rq->used_day, rq->used_month, *user, qc, wallMillis());
                                    out["quota"] = std::move(quota);
                                } else {
                                    // 配额状态不可用：账本数据照常返回，去掉 quota 字段
                                    SPDLOG_WARN("usage api: quota status unavailable: {}", rq.error().message);
                                }
                                writeJson(conn, 200, out.dump());
                            });
            });
}

void UsageApi::handleUsageRequests(const http::HttpRequest& request, const net::TcpConnectionPtr& conn,
                                   std::shared_ptr<const UserConfig> user) {
    if (context_.ledger_writer == nullptr) {
        writeJson(conn, 503,
                  proto::errorBody(proto::Protocol::openai, "usage ledger not configured", "service_unavailable"));
        return;
    }
    std::size_t limit = 50;
    if (auto l = queryParam(request.query, "limit")) {
        unsigned long v = std::strtoul(l->c_str(), nullptr, 10);
        if (v > 0) limit = std::min<unsigned long>(v, 500);
    }
    std::optional<std::string> before = queryParam(request.query, "before");

    auto weak_conn = std::weak_ptr<net::TcpConnection>(conn);
    pgQuery(conn, "usage_records_page", store::PgParams{user->name, before, std::to_string(limit)},
            [this, weak_conn](std::expected<store::PgResult, store::PgError> r) {
                auto conn = weak_conn.lock();
                if (!conn) return;
                if (!r) {
                    writeJson(conn, 503, proto::errorBody(proto::Protocol::openai,
                                                          "usage query failed: " + r.error().message,
                                                          "service_unavailable"));
                    return;
                }
                nlohmann::json out;
                out["requests"] = nlohmann::json::array();
                for (const auto& row : r->rows) out["requests"].push_back(rowToRecordJson(row));
                writeJson(conn, 200, out.dump());
            });
}

void UsageApi::handleAdminUsage(const http::HttpRequest& request, const net::TcpConnectionPtr& conn) {
    if (context_.ledger_writer == nullptr) {
        writeJson(conn, 503,
                  proto::errorBody(proto::Protocol::openai, "usage ledger not configured", "service_unavailable"));
        return;
    }
    auto key = queryParam(request.query, "key");
    const auto& qc = context_.config->quota;
    int64_t now = wallMillis();
    std::string start = queryParam(request.query, "start").value_or(quotaBillingDay(now, qc.utc_offset_minutes));
    std::string end = queryParam(request.query, "end").value_or(start);

    auto weak_conn = std::weak_ptr<net::TcpConnection>(conn);
    store::PgParams params;
    if (key) params.push_back(*key);
    params.push_back(start);
    params.push_back(end);
    pgQuery(conn, key ? "usage_daily_by_key" : "usage_daily_grouped_by_key", std::move(params),
            [this, weak_conn, with_key = key.has_value()](std::expected<store::PgResult, store::PgError> r) {
                auto conn = weak_conn.lock();
                if (!conn) return;
                if (!r) {
                    writeJson(conn, 503, proto::errorBody(proto::Protocol::openai,
                                                          "usage query failed: " + r.error().message,
                                                          "service_unavailable"));
                    return;
                }
                nlohmann::json out;
                nlohmann::json days = nlohmann::json::array();
                for (const auto& row : r->rows) days.push_back(rowToDailyJson(row, with_key));
                out["days"] = days;
                out["total"] = dailyTotals(days);
                writeJson(conn, 200, out.dump());
            });
}

void UsageApi::handleAdminUsageRequests(const http::HttpRequest& request, const net::TcpConnectionPtr& conn) {
    if (context_.ledger_writer == nullptr) {
        writeJson(conn, 503,
                  proto::errorBody(proto::Protocol::openai, "usage ledger not configured", "service_unavailable"));
        return;
    }
    auto key = queryParam(request.query, "key");
    if (!key) {
        writeJson(conn, 400,
                  proto::errorBody(proto::Protocol::openai, "missing key parameter", "invalid_request_error"));
        return;
    }
    std::size_t limit = 50;
    if (auto l = queryParam(request.query, "limit")) {
        unsigned long v = std::strtoul(l->c_str(), nullptr, 10);
        if (v > 0) limit = std::min<unsigned long>(v, 500);
    }
    std::optional<std::string> before = queryParam(request.query, "before");

    auto weak_conn = std::weak_ptr<net::TcpConnection>(conn);
    pgQuery(conn, "usage_records_page", store::PgParams{*key, before, std::to_string(limit)},
            [this, weak_conn](std::expected<store::PgResult, store::PgError> r) {
                auto conn = weak_conn.lock();
                if (!conn) return;
                if (!r) {
                    writeJson(conn, 503, proto::errorBody(proto::Protocol::openai,
                                                          "usage query failed: " + r.error().message,
                                                          "service_unavailable"));
                    return;
                }
                nlohmann::json out;
                out["requests"] = nlohmann::json::array();
                for (const auto& row : r->rows) out["requests"].push_back(rowToRecordJson(row));
                writeJson(conn, 200, out.dump());
            });
}

void UsageApi::handleReconcile(const http::HttpRequest& request, const net::TcpConnectionPtr& conn,
                               std::optional<std::string> key) {
    if (context_.ledger_writer == nullptr || quota_ == nullptr) {
        writeJson(conn, 503,
                  proto::errorBody(proto::Protocol::openai, "ledger/quota not configured", "service_unavailable"));
        return;
    }
    if (!key) {
        writeJson(conn, 400,
                  proto::errorBody(proto::Protocol::openai, "missing key parameter", "invalid_request_error"));
        return;
    }
    const auto& qc = context_.config->quota;
    int64_t now = wallMillis();
    std::string day = quotaBillingDay(now, qc.utc_offset_minutes);
    // 月第一天（本地）
    std::time_t secs = static_cast<std::time_t>(now / 1000 + static_cast<int64_t>(qc.utc_offset_minutes) * 60);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char month_first[16];
    std::snprintf(month_first, sizeof(month_first), "%04d-%02d-01", tm.tm_year + 1900, tm.tm_mon + 1);

    auto weak_conn = std::weak_ptr<net::TcpConnection>(conn);
    // 先取 PG 日/月汇总（两次查询串行），再与 Redis 计数对比
    pgQuery(conn, "usage_day_sum", {*key, day},
            [this, weak_conn, key = *key, month_first, day, qc](std::expected<store::PgResult, store::PgError> rd) {
                auto conn = weak_conn.lock();
                if (!conn) return;
                if (!rd) {
                    writeJson(conn, 503, proto::errorBody(proto::Protocol::openai,
                                                          "usage query failed: " + rd.error().message,
                                                          "service_unavailable"));
                    return;
                }
                int64_t pg_day = parseNum(rd->rows.empty() ? std::nullopt : rd->rows[0].columns[0]).value_or(0);
                pgQuery(conn, "usage_month_sum", {key, month_first},
                        [this, weak_conn, key, day, pg_day, qc](std::expected<store::PgResult, store::PgError> rm) {
                            auto conn = weak_conn.lock();
                            if (!conn) return;
                            if (!rm) {
                                writeJson(conn, 503, proto::errorBody(proto::Protocol::openai,
                                                                      "usage query failed: " + rm.error().message,
                                                                      "service_unavailable"));
                                return;
                            }
                            int64_t pg_month =
                                parseNum(rm->rows.empty() ? std::nullopt : rm->rows[0].columns[0]).value_or(0);
                            auto* redis = redisOf(conn);
                            if (redis == nullptr) {
                                writeJson(conn, 503, proto::errorBody(proto::Protocol::openai,
                                                                      "redis not configured on this loop",
                                                                      "service_unavailable"));
                                return;
                            }
                            int64_t now = wallMillis();
                            int64_t day_num = quotaDayOf(now, qc.utc_offset_minutes);
                            int64_t month_num = quotaMonthOf(now, qc.utc_offset_minutes);
                            quota_->get(*redis, key, day_num, month_num,
                                        [this, weak_conn, key, day, pg_day, pg_month](
                                            std::expected<QuotaUsage, store::RedisError> rq) {
                                            auto conn = weak_conn.lock();
                                            if (!conn) return;
                                            if (!rq) {
                                                writeJson(conn, 503,
                                                          proto::errorBody(proto::Protocol::openai,
                                                                           "redis unavailable: " +
                                                                               rq.error().message,
                                                                           "service_unavailable"));
                                                return;
                                            }
                                            nlohmann::json out;
                                            out["key"] = key;
                                            out["day"] = day;
                                            out["redis"] = {{"used_day", rq->used_day},
                                                            {"used_month", rq->used_month}};
                                            out["postgres"] = {{"billable_day", pg_day}, {"billable_month", pg_month}};
                                            out["diff"] = {{"day", rq->used_day - pg_day},
                                                           {"month", rq->used_month - pg_month}};
                                            writeJson(conn, 200, out.dump());
                                        });
                        });
            });
}

}  // namespace gw::gateway
