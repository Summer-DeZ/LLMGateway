#include "gateway/config.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <set>

#include "base/strings.hpp"

namespace gw::gateway {

namespace {

using nlohmann::json;

std::expected<json, std::string> loadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::unexpected("cannot open config file: " + path);
    try {
        json j;
        in >> j;
        return j;
    } catch (const std::exception& e) {
        return std::unexpected(std::string("config parse error: ") + e.what());
    }
}

void substituteEnv(json& j, std::set<std::string>& missing) {
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        if (s.starts_with("env:")) {
            std::string name = s.substr(4);
            const char* value = std::getenv(name.c_str());
            if (value == nullptr) {
                missing.insert(name);
            } else {
                j = std::string(value);
            }
        }
        return;
    }
    if (j.is_object()) {
        for (auto& [key, value] : j.items()) substituteEnv(value, missing);
    } else if (j.is_array()) {
        for (auto& value : j) substituteEnv(value, missing);
    }
}

std::chrono::milliseconds msOr(const json& j, const char* key, std::chrono::milliseconds def) {
    if (!j.contains(key)) return def;
    return std::chrono::milliseconds(j.at(key).get<std::int64_t>());
}

std::optional<std::uint16_t> parsePortText(const std::string& text) {
    unsigned value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0 || value > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::optional<std::pair<std::string, std::uint16_t>> parseListenAddress(const std::string& text) {
    if (text.empty()) return std::nullopt;
    if (text.front() == '[') {
        auto close = text.find(']');
        if (close == std::string::npos) return std::nullopt;
        std::string host = text.substr(1, close - 1);
        if (close + 1 >= text.size() || text[close + 1] != ':') return std::nullopt;
        auto port = parsePortText(text.substr(close + 2));
        if (!port) return std::nullopt;
        return std::make_pair(host, *port);
    }
    auto colon = text.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string host = text.substr(0, colon);
    auto port = parsePortText(text.substr(colon + 1));
    if (!port || host.empty()) return std::nullopt;
    return std::make_pair(host, *port);
}

std::expected<std::shared_ptr<Config>, std::string> Config::load(const std::string& path) {
    auto parsed = loadFile(path);
    if (!parsed) return std::unexpected(parsed.error());
    json j = *parsed;

    std::set<std::string> missing;
    substituteEnv(j, missing);
    if (!missing.empty()) {
        std::string names;
        for (const auto& n : missing) {
            if (!names.empty()) names += ", ";
            names += n;
        }
        return std::unexpected("environment variable(s) not set: " + names);
    }

    auto cfg = std::make_shared<Config>();
    try {
        if (j.contains("server")) {
            const json& s = j.at("server");
            cfg->server.listen = s.value("listen", cfg->server.listen);
            cfg->server.io_threads = s.value("io_threads", cfg->server.io_threads);
            cfg->server.max_body_bytes = s.value("max_body_bytes", cfg->server.max_body_bytes);
            cfg->server.header_timeout = msOr(s, "header_timeout_ms", cfg->server.header_timeout);
            cfg->server.body_timeout = msOr(s, "body_timeout_ms", cfg->server.body_timeout);
            cfg->server.keepalive_idle = msOr(s, "keepalive_idle_ms", cfg->server.keepalive_idle);
        }
        if (j.contains("admin")) {
            cfg->admin.listen_local_only = j.at("admin").value("listen_local_only", true);
        }
        if (j.contains("shutdown")) {
            cfg->shutdown.drain_seconds = j.at("shutdown").value("drain_seconds", 30);
        }
        if (j.contains("log")) {
            const json& l = j.at("log");
            cfg->log.level = l.value("level", cfg->log.level);
            cfg->log.file = l.value("file", cfg->log.file);
            cfg->log.access_file = l.value("access_file", cfg->log.access_file);
            cfg->log.slow = msOr(l, "slow_ms", cfg->log.slow);
        }
        if (j.contains("stream")) {
            const json& s = j.at("stream");
            cfg->stream.high_water_bytes = s.value("high_water_bytes", cfg->stream.high_water_bytes);
            cfg->stream.stall = msOr(s, "stall_ms", cfg->stream.stall);
            cfg->stream.downstream_sndbuf_bytes =
                s.value("downstream_sndbuf_bytes", cfg->stream.downstream_sndbuf_bytes);
        }
        if (j.contains("dns")) {
            cfg->dns.cache_ttl_seconds = j.at("dns").value("cache_ttl_seconds", 60);
        }
        if (j.contains("cache")) {
            const json& c = j.at("cache");
            cfg->cache.enabled = c.value("enabled", true);
            cfg->cache.max_bytes = c.value("max_bytes", cfg->cache.max_bytes);
            cfg->cache.max_entry_bytes = c.value("max_entry_bytes", cfg->cache.max_entry_bytes);
            cfg->cache.ttl_seconds = c.value("ttl_seconds", cfg->cache.ttl_seconds);
        }
        cfg->instance_id = j.value("instance_id", cfg->instance_id);

        if (j.contains("users")) {
            for (const auto& [key, u] : j.at("users").items()) {
                UserConfig user;
                user.key = key;
                user.name = u.value("name", key);
                user.rpm = u.value("rpm", 0.0);
                user.tpm = u.value("tpm", 0.0);
                if (u.contains("quota")) {
                    QuotaConfig q;
                    q.daily_tokens = u.at("quota").value("daily_tokens", std::int64_t{0});
                    q.monthly_tokens = u.at("quota").value("monthly_tokens", std::int64_t{0});
                    user.quota = q;
                }
                cfg->users.emplace(key, std::move(user));
            }
        }

        if (j.contains("upstreams")) {
            for (const auto& [name, u] : j.at("upstreams").items()) {
                UpstreamConfig up;
                up.name = name;
                up.kind = u.at("kind").get<std::string>();
                if (up.kind != "openai-chat" && up.kind != "anthropic") {
                    return std::unexpected("upstream " + name + ": unknown kind " + up.kind);
                }
                std::string base_url = u.at("base_url").get<std::string>();
                if (base_url.starts_with("https://")) {
                    return std::unexpected("upstream " + name + ": https is not supported (local plaintext upstreams only): " +
                                           base_url);
                }
                auto url = http::parseBaseUrl(base_url);
                if (!url) return std::unexpected("upstream " + name + ": invalid base_url " + base_url);
                up.url = *url;
                up.model = u.value("model", "");
                up.api_key = u.value("api_key", "");
                up.metering = u.value("metering", "final");
                if (up.metering != "llamacpp" && up.metering != "vllm" && up.metering != "final") {
                    return std::unexpected("upstream " + name + ": unknown metering " + up.metering);
                }
                if (up.metering != "final" && up.kind != "openai-chat") {
                    return std::unexpected("upstream " + name + ": metering " + up.metering +
                                           " requires kind openai-chat");
                }
                up.count_tokens = u.value("count_tokens", false);
                up.default_max_tokens = u.value("default_max_tokens", 8192);
                up.pool_idle_seconds = u.value("pool_idle_seconds", 4);
                if (u.contains("extra_body")) up.extra_body = u.at("extra_body");
                if (u.contains("timeouts")) {
                    const json& t = u.at("timeouts");
                    up.timeouts.connect = msOr(t, "connect_ms", up.timeouts.connect);
                    up.timeouts.ttft = msOr(t, "ttft_ms", up.timeouts.ttft);
                    up.timeouts.idle = msOr(t, "idle_ms", up.timeouts.idle);
                    up.timeouts.total = msOr(t, "total_ms", up.timeouts.total);
                    up.timeouts.precommit = msOr(t, "precommit_ms", up.timeouts.precommit);
                }
                if (u.contains("breaker")) {
                    const json& b = u.at("breaker");
                    up.breaker.failure_threshold = b.value("failure_threshold", 5);
                    up.breaker.cooldown = msOr(b, "cooldown_ms", up.breaker.cooldown);
                }
                cfg->upstreams.emplace(name, std::move(up));
            }
        }

        if (j.contains("models")) {
            for (const auto& [alias, m] : j.at("models").items()) {
                ModelConfig model;
                model.alias = alias;
                model.targets = m.at("targets").get<std::vector<std::string>>();
                if (model.targets.empty()) {
                    return std::unexpected("model " + alias + ": empty targets");
                }
                for (const auto& target : model.targets) {
                    if (cfg->upstreams.find(target) == cfg->upstreams.end()) {
                        return std::unexpected("model " + alias + ": target " + target + " not found in upstreams");
                    }
                }
                model.rpm = m.value("rpm", 0.0);
                model.tpm = m.value("tpm", 0.0);
                model.default_max_tokens = m.value("default_max_tokens", 4096);
                if (m.contains("retry")) {
                    const json& r = m.at("retry");
                    model.retry.max_attempts = r.value("max_attempts", 2);
                    model.retry.base_ms = r.value("base_ms", 500);
                    model.retry.max_ms = r.value("max_ms", 8000);
                    model.retry.max_retry_after_ms = r.value("max_retry_after_ms", 10000);
                }
                cfg->model_order.push_back(alias);
                cfg->models.emplace(alias, std::move(model));
            }
        }

        if (j.contains("pricing")) {
            for (const auto& [name, p] : j.at("pricing").items()) {
                PricingEntry entry;
                if (p.contains("input")) entry.input = p.at("input").get<double>();
                if (p.contains("output")) entry.output = p.at("output").get<double>();
                if (p.contains("cached")) entry.cached = p.at("cached").get<double>();
                cfg->pricing.emplace(name, entry);
            }
        }

        if (j.contains("postgres")) {
            PostgresConfig pg;
            pg.dsn = j.at("postgres").at("dsn").get<std::string>();
            pg.connect_timeout = msOr(j.at("postgres"), "connect_timeout_ms", pg.connect_timeout);
            cfg->postgres = pg;
        }
        if (j.contains("redis")) {
            RedisConfig r;
            const json& rj = j.at("redis");
            r.host = rj.value("host", r.host);
            r.port = static_cast<std::uint16_t>(rj.value("port", 6379));
            r.password = rj.value("password", "");
            r.command_timeout = msOr(rj, "command_timeout_ms", r.command_timeout);
            cfg->redis = r;
        }
        if (j.contains("ledger")) {
            const json& l = j.at("ledger");
            cfg->ledger.max_backlog = l.value("max_backlog", cfg->ledger.max_backlog);
            cfg->ledger.batch_rows = l.value("batch_rows", cfg->ledger.batch_rows);
            cfg->ledger.batch_interval = msOr(l, "batch_interval_ms", cfg->ledger.batch_interval);
        }
        if (j.contains("quota")) {
            const json& q = j.at("quota");
            cfg->quota.utc_offset_minutes = q.value("utc_offset_minutes", 480);
            cfg->quota.report_every_tokens = q.value("report_every_tokens", 32);
            cfg->quota.report_interval = msOr(q, "report_interval_ms", cfg->quota.report_interval);
            cfg->quota.on_redis_failure = q.value("on_redis_failure", "reject");
            if (cfg->quota.on_redis_failure != "reject" && cfg->quota.on_redis_failure != "allow") {
                return std::unexpected("quota.on_redis_failure must be reject or allow");
            }
        }

        bool any_quota = false;
        for (const auto& [key, user] : cfg->users) {
            if (user.quota) any_quota = true;
        }
        if (any_quota && !cfg->redis) {
            return std::unexpected("a user has quota configured but no redis section");
        }
        if (cfg->redis && !cfg->postgres) {
            return std::unexpected("redis section requires postgres section (quota rebuild)");
        }
        if (!parseListenAddress(cfg->server.listen)) {
            return std::unexpected("server.listen is invalid: " + cfg->server.listen);
        }
        if (cfg->server.io_threads < 1) {
            return std::unexpected("server.io_threads must be >= 1");
        }
    } catch (const std::exception& e) {
        return std::unexpected(std::string("config field error: ") + e.what());
    }
    return cfg;
}

}  // namespace gw::gateway
