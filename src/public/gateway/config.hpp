#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/url.hpp"

namespace gw::gateway {

struct QuotaConfig {
    std::int64_t daily_tokens = 0;
    std::int64_t monthly_tokens = 0;
};

struct UserConfig {
    std::string key;
    std::string name;
    double rpm = 0;
    double tpm = 0;
    std::optional<double> rpm_burst;  // 缺省 = rpm
    std::optional<double> tpm_burst;  // 缺省 = tpm
    std::optional<QuotaConfig> quota;
};

struct Timeouts {
    std::chrono::milliseconds connect{10000};
    std::chrono::milliseconds ttft{90000};
    std::chrono::milliseconds idle{60000};
    std::chrono::milliseconds total{600000};
    std::chrono::milliseconds precommit{100000};
};

struct BreakerConfig {
    int failure_threshold = 5;
    std::chrono::milliseconds cooldown{30000};
};

struct UpstreamConfig {
    std::string name;
    std::string kind;  // openai-chat | anthropic
    http::UrlParts url;
    std::string model;
    std::string api_key;
    std::string metering = "final";
    bool count_tokens = false;
    int default_max_tokens = 8192;
    int pool_idle_seconds = 4;
    nlohmann::json extra_body;
    Timeouts timeouts;
    BreakerConfig breaker;
};

struct RetryConfig {
    int max_attempts = 2;
    int base_ms = 500;
    int max_ms = 8000;
    int max_retry_after_ms = 10000;
};

struct ModelConfig {
    std::string alias;
    std::vector<std::string> targets;
    double rpm = 0;
    double tpm = 0;
    std::optional<double> rpm_burst;
    std::optional<double> tpm_burst;
    int default_max_tokens = 4096;
    RetryConfig retry;
};

struct PricingEntry {
    std::optional<double> input;
    std::optional<double> output;
    std::optional<double> cached;
};

struct ServerConfig {
    std::string listen = "0.0.0.0:8080";
    int io_threads = 8;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::chrono::milliseconds header_timeout{30000};
    std::chrono::milliseconds body_timeout{60000};
    std::chrono::milliseconds keepalive_idle{75000};
};

struct AdminConfig {
    bool listen_local_only = true;
};

struct ShutdownConfig {
    int drain_seconds = 30;
};

struct LogConfig {
    std::string level = "info";
    std::string file = "logs/gateway.log";
    std::string access_file = "logs/access.log";
    std::chrono::milliseconds slow{30000};
};

struct StreamConfig {
    std::size_t high_water_bytes = 262144;
    std::chrono::milliseconds stall{60000};
    // 下游连接的 SO_SNDBUF；0 表示不设置（交给内核自动调优）
    int downstream_sndbuf_bytes = 65536;
};

struct DnsConfig {
    int cache_ttl_seconds = 60;
};

struct CacheConfig {
    bool enabled = true;
    std::size_t max_bytes = 268435456;
    std::size_t max_entry_bytes = 4194304;
    int ttl_seconds = 600;
};

struct PostgresConfig {
    std::string dsn;
    std::chrono::milliseconds connect_timeout{3000};
};

struct RedisConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string password;
    std::chrono::milliseconds command_timeout{50};
};

struct LedgerConfig {
    std::size_t max_backlog = 100000;
    int batch_rows = 500;
    std::chrono::milliseconds batch_interval{200};
};

struct QuotaRuntimeConfig {
    int utc_offset_minutes = 480;
    int report_every_tokens = 32;
    std::chrono::milliseconds report_interval{100};
    std::string on_redis_failure = "reject";  // reject | allow
};

struct Config {
    ServerConfig server;
    AdminConfig admin;
    bool management_enabled = false;
    ShutdownConfig shutdown;
    LogConfig log;
    StreamConfig stream;
    DnsConfig dns;
    CacheConfig cache;

    std::unordered_map<std::string, UserConfig> users;      // api key -> user
    std::unordered_map<std::string, UpstreamConfig> upstreams;
    std::unordered_map<std::string, ModelConfig> models;
    std::vector<std::string> model_order;
    std::unordered_map<std::string, PricingEntry> pricing;

    std::string instance_id = "gw-1";
    std::optional<PostgresConfig> postgres;
    std::optional<RedisConfig> redis;
    LedgerConfig ledger;
    QuotaRuntimeConfig quota;

    static std::expected<std::shared_ptr<Config>, std::string> load(const std::string& path);

    const UserConfig* findUser(const std::string& key) const {
        auto it = users.find(key);
        return it == users.end() ? nullptr : &it->second;
    }
    const UpstreamConfig* findUpstream(const std::string& name) const {
        auto it = upstreams.find(name);
        return it == upstreams.end() ? nullptr : &it->second;
    }
    const ModelConfig* findModel(const std::string& alias) const {
        auto it = models.find(alias);
        return it == models.end() ? nullptr : &it->second;
    }
    const PricingEntry* findPricing(const std::string& upstream) const {
        auto it = pricing.find(upstream);
        return it == pricing.end() ? nullptr : &it->second;
    }
};

// 解析 "host:port" 形式的监听地址（支持 IPv6 字面量 [::1]:8080）
std::optional<std::pair<std::string, std::uint16_t>> parseListenAddress(const std::string& text);

}  // namespace gw::gateway
