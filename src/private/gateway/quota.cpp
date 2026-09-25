#include "gateway/quota.hpp"

#include <ctime>

#include "base/log.hpp"
#include "gateway/metrics.hpp"

namespace gw::gateway {

namespace {

// 把 epoch 毫秒换算成"本地日历"（UTC + offset），复用 gmtime
void localCalendar(std::int64_t wall_ms, int utc_offset_minutes, std::tm& tm) {
    std::time_t secs = static_cast<std::time_t>(wall_ms / 1000 + static_cast<std::int64_t>(utc_offset_minutes) * 60);
    gmtime_r(&secs, &tm);
}

}  // namespace

std::int64_t quotaDayOf(std::int64_t wall_ms, int utc_offset_minutes) {
    std::tm tm{};
    localCalendar(wall_ms, utc_offset_minutes, tm);
    return static_cast<std::int64_t>((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}

std::int64_t quotaMonthOf(std::int64_t wall_ms, int utc_offset_minutes) {
    std::tm tm{};
    localCalendar(wall_ms, utc_offset_minutes, tm);
    return static_cast<std::int64_t>((tm.tm_year + 1900) * 100 + (tm.tm_mon + 1));
}

std::string quotaBillingDay(std::int64_t wall_ms, int utc_offset_minutes) {
    std::tm tm{};
    localCalendar(wall_ms, utc_offset_minutes, tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string quotaDayKey(const std::string& key_name, std::int64_t day) {
    // 花括号是 Redis Cluster hash tag：同一 key 的日/月计数落同一槽
    return "gw:quota:{" + key_name + "}:d:" + std::to_string(day);
}

std::string quotaMonthKey(const std::string& key_name, std::int64_t month) {
    return "gw:quota:{" + key_name + "}:m:" + std::to_string(month);
}

std::int64_t secondsToNextLocalMidnight(std::int64_t wall_ms, int utc_offset_minutes) {
    std::int64_t sec_of_day =
        ((wall_ms / 1000 + static_cast<std::int64_t>(utc_offset_minutes) * 60) % 86400 + 86400) % 86400;
    return 86400 - sec_of_day;
}

bool QuotaView::shouldReport(std::int64_t now_ms, int report_every_tokens, int report_interval_ms) const {
    if (consumed - reported >= report_every_tokens) return true;
    return now_ms - last_report_ms >= report_interval_ms;
}

// ===== QuotaManager =====

QuotaManager::QuotaManager(const Config& config, Metrics* metrics) : config_(&config), metrics_(metrics) {}

QuotaManager::ScriptShas& QuotaManager::shasOf(store::RedisClient& client) {
    std::scoped_lock lock(shas_mu_);
    return shas_[&client];
}

void QuotaManager::get(store::RedisClient& client, const std::string& key_name, std::int64_t day,
                       std::int64_t month, std::function<void(std::expected<QuotaUsage, store::RedisError>)> cb) {
    auto keys = std::vector<std::string>{quotaDayKey(key_name, day), quotaMonthKey(key_name, month)};
    auto& shas = shasOf(client);
    eval(client, kQuotaGetScript, shas.quota_get, keys, {}, std::move(cb), false);
}

void QuotaManager::add(store::RedisClient& client, const std::string& key_name, std::int64_t day,
                       std::int64_t month, std::int64_t delta,
                       std::function<void(std::expected<QuotaUsage, store::RedisError>)> cb) {
    auto keys = std::vector<std::string>{quotaDayKey(key_name, day), quotaMonthKey(key_name, month)};
    auto args = std::vector<std::string>{std::to_string(delta), std::to_string(kQuotaDayTtlSeconds),
                                         std::to_string(kQuotaMonthTtlSeconds)};
    auto& shas = shasOf(client);
    eval(client, kQuotaAddScript, shas.quota_add, keys, std::move(args), std::move(cb), false);
}

void QuotaManager::eval(store::RedisClient& client, const std::string& script,
                        const std::optional<std::string>& sha, std::vector<std::string> keys,
                        std::vector<std::string> args,
                        std::function<void(std::expected<QuotaUsage, store::RedisError>)> cb, bool retried) {
    std::vector<std::string> cmd;
    if (sha) {
        cmd = {"EVALSHA", *sha, "2"};
    } else {
        cmd = {"EVAL", script, "2"};
    }
    cmd.insert(cmd.end(), keys.begin(), keys.end());
    cmd.insert(cmd.end(), args.begin(), args.end());

    if (metrics_) metrics_->observeRedisCommand(sha ? "evalsha" : "eval");
    auto started = nowSteady();
    client.command(
        std::move(cmd),
        [this, &client, script, sha_slot = &shasOf(client).quota_get, keys, args, cb, retried, started](
            store::RedisClient::Reply r) {
            if (metrics_) metrics_->observeRedisLatency(nowSteady() - started);
            if (r) {
                if (const std::string* err = store::asError(*r)) {
                    if (err->rfind("NOSCRIPT", 0) == 0 && !retried) {
                        // 脚本被清（Redis 重建）：重新加载后重试一次
                        client.command({"SCRIPT", "LOAD", script},
                                       [sha_slot](store::RedisClient::Reply lr) {
                                           if (lr) {
                                               if (lr->isBulk() && !lr->isNull()) {
                                                   *sha_slot = lr->str();
                                               }
                                           }
                                       });
                        eval(client, script, *sha_slot, keys, args, std::move(cb), true);
                        return;
                    }
                    if (metrics_) metrics_->observeRedisError("error_reply");
                    cb(std::unexpected(store::RedisError{store::RedisErrorKind::connection, "redis: " + *err}));
                    return;
                }
                const auto* arr = store::asArray(*r);
                if (arr != nullptr && arr->size() >= 2) {
                    auto d = store::valueToInt((*arr)[0]);
                    auto m = store::valueToInt((*arr)[1]);
                    if (d && m) {
                        cb(QuotaUsage{*d, *m});
                        return;
                    }
                }
                cb(std::unexpected(store::RedisError{store::RedisErrorKind::connection, "bad quota reply"}));
                return;
            }
            if (metrics_) {
                metrics_->observeRedisError(r.error().kind == store::RedisErrorKind::timeout ? "timeout"
                                                                                             : "connection");
            }
            cb(std::unexpected(r.error()));
        });
}

}  // namespace gw::gateway
