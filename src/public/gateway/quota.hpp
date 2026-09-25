#pragma once

#include <cstdint>
#include <functional>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "gateway/config.hpp"
#include "store/redis_client.hpp"

namespace gw::gateway {

class Metrics;

// ===== 周期与键名（usage-ledger.md §5.1）=====

// 按请求开始时刻（wall 毫秒）与 UTC 偏移（分钟）计算所属日/月
std::int64_t quotaDayOf(std::int64_t wall_ms, int utc_offset_minutes);    // YYYYMMDD
std::int64_t quotaMonthOf(std::int64_t wall_ms, int utc_offset_minutes);  // YYYYMM
// 与 Redis 计数/账本 billing_day 用的同一个日期（本地日 → YYYY-MM-DD）
std::string quotaBillingDay(std::int64_t wall_ms, int utc_offset_minutes);

std::string quotaDayKey(const std::string& key_name, std::int64_t day);    // gw:quota:{name}:d:YYYYMMDD
std::string quotaMonthKey(const std::string& key_name, std::int64_t month);  // gw:quota:{name}:m:YYYYMM

// Redis 过期：日 3 天、月 40 天
constexpr std::int64_t kQuotaDayTtlSeconds = 3 * 86400;
constexpr std::int64_t kQuotaMonthTtlSeconds = 40 * 86400;

// 距下个本地零点的秒数（准入 429 的 Retry-After）
std::int64_t secondsToNextLocalMidnight(std::int64_t wall_ms, int utc_offset_minutes);

// ===== Lua 脚本（usage-ledger.md §5.2）=====
// quota_add（KEYS: day, month；ARGV: delta, day_ttl, month_ttl）→ {d, m}
inline constexpr const char* kQuotaAddScript = R"lua(
local d = redis.call('INCRBY', KEYS[1], ARGV[1])
local m = redis.call('INCRBY', KEYS[2], ARGV[1])
if d == tonumber(ARGV[1]) then redis.call('EXPIRE', KEYS[1], ARGV[2]) end
if m == tonumber(ARGV[1]) then redis.call('EXPIRE', KEYS[2], ARGV[3]) end
return {d, m}
)lua";
// quota_get（KEYS: day, month）→ {day_used, month_used}
inline constexpr const char* kQuotaGetScript = R"lua(
return {tonumber(redis.call('GET', KEYS[1]) or '0'), tonumber(redis.call('GET', KEYS[2]) or '0')}
)lua";

// ===== 会话侧的上报与截断判断（usage-ledger.md §5.4）=====
// 只在所属 io loop 上使用，不加锁。remaining 为 nullopt 表示不限（未配置 quota 或 Redis allow 模式）。
struct QuotaView {
    std::optional<std::int64_t> remaining;  // 最近一次得知的剩余额度
    std::int64_t reported = 0;              // 已经 quota_add 到 Redis 的量
    std::int64_t consumed = 0;              // 各尝试已知 consumed 之和
    std::int64_t projected = 0;             // 各尝试已知 projected 之和（≥ consumed）
    std::int64_t last_report_ms = 0;
    bool truncated = false;

    // meter 更新后调用：返回 true 表示超额（未提交 → 429；已提交 → 截断）
    bool overBudget() const { return remaining.has_value() && projected - reported > *remaining; }
    // 是否该上报：consumed - reported ≥ report_every 或距上次上报超过 interval_ms
    bool shouldReport(std::int64_t now_ms, int report_every_tokens, int report_interval_ms) const;
};

// ===== Redis 配额访问 =====

struct QuotaUsage {
    std::int64_t used_day = 0;
    std::int64_t used_month = 0;
};

// EVALSHA 封装：按 RedisClient 连接缓存 SCRIPT LOAD 的 SHA，收到 NOSCRIPT 时重新加载后重试一次。
// QuotaManager 本身不加锁：它的方法都在"使用者的 loop"上调用，各 loop 只碰自己的 SHA 缓存。
// （SHA 缓存表只在各 loop 上插入自己的键，主线程不访问。）
class QuotaManager : noncopyable {
public:
    explicit QuotaManager(const Config& config, Metrics* metrics = nullptr);

    void get(store::RedisClient& client, const std::string& key_name, std::int64_t day, std::int64_t month,
             std::function<void(std::expected<QuotaUsage, store::RedisError>)> cb);
    void add(store::RedisClient& client, const std::string& key_name, std::int64_t day, std::int64_t month,
             std::int64_t delta, std::function<void(std::expected<QuotaUsage, store::RedisError>)> cb);

private:
    struct ScriptShas {
        std::optional<std::string> quota_add;
        std::optional<std::string> quota_get;
    };
    ScriptShas& shasOf(store::RedisClient& client);
    void eval(store::RedisClient& client, const std::string& script, const std::optional<std::string>& sha,
              std::vector<std::string> keys, std::vector<std::string> args,
              std::function<void(std::expected<QuotaUsage, store::RedisError>)> cb, bool retried);

    const Config* config_;
    Metrics* metrics_ = nullptr;
    std::mutex shas_mu_;  // shas_ 表会被多个 io loop 插入各自的条目
    std::map<store::RedisClient*, ScriptShas> shas_;
};

}  // namespace gw::gateway
