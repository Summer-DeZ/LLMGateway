#include "gateway/metrics.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>

#include <nlohmann/json.hpp>

#include "gateway/config.hpp"
#include "gateway/proxy_session.hpp"
#include "gateway/pricing.hpp"
#include "gateway/request_record.hpp"

namespace gw::gateway {

namespace {

constexpr std::array<std::string_view, 4> kTokenTypes = {"prompt", "cached", "prompt_computed", "completion"};
constexpr std::array<std::string_view, 3> kAttemptResults = {"ok", "retryable", "fatal"};

std::string escapeLabel(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '"': out.append("\\\""); break;
            case '\n': out.append("\\n"); break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string labelPair(std::string_view name, std::string_view value) {
    std::string out;
    out.append(name);
    out.append("=\"");
    out.append(escapeLabel(value));
    out.push_back('"');
    return out;
}

template <typename... Rest>
void appendLabels(std::string& out, std::string_view name, std::string_view value, Rest&&... rest) {
    if (!out.empty()) out.push_back(',');
    out.append(labelPair(name, value));
    if constexpr (sizeof...(rest) > 0) appendLabels(out, rest...);
}

template <typename... Args>
std::string renderLabels(Args&&... args) {
    std::string out;
    appendLabels(out, std::forward<Args>(args)...);
    return out;
}

std::string joinKey(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (std::string_view p : parts) {
        if (!out.empty()) out.push_back('\x1f');
        out.append(p);
    }
    return out;
}

std::string fmtDouble(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

// 直方图：每桶一个原子计数，sum 用微秒整数（输出时换算成秒）
struct Hist {
    std::vector<double> bounds;
    std::unique_ptr<std::atomic<std::uint64_t>[]> counts;
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> sum_us{0};

    explicit Hist(std::vector<double> b) : bounds(std::move(b)) {
        counts.reset(new std::atomic<std::uint64_t>[bounds.size()]);
        for (std::size_t i = 0; i < bounds.size(); ++i) counts[i].store(0, std::memory_order_relaxed);
    }

    void observe(double seconds) {
        std::size_t i = 0;
        while (i + 1 < bounds.size() && seconds > bounds[i]) ++i;
        counts[i].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        if (seconds < 0) seconds = 0;
        sum_us.fetch_add(static_cast<std::uint64_t>(seconds * 1e6), std::memory_order_relaxed);
    }

    double meanSeconds() const {
        std::uint64_t n = count.load(std::memory_order_relaxed);
        if (n == 0) return 0;
        return static_cast<double>(sum_us.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e6;
    }

    // 由累计分布线性插值估算分位数
    double quantile(double q) const {
        std::uint64_t total = count.load(std::memory_order_relaxed);
        if (total == 0) return 0;
        double rank = q * static_cast<double>(total);
        double cum = 0;
        double prev = 0;
        for (std::size_t i = 0; i < bounds.size(); ++i) {
            double c = cum + static_cast<double>(counts[i].load(std::memory_order_relaxed));
            if (c >= rank) {
                if (c == cum) return bounds[i];
                return prev + (bounds[i] - prev) * (rank - cum) / (c - cum);
            }
            cum = c;
            prev = bounds[i];
        }
        return prev;
    }
};

const char* circuitStateName(int state) {
    switch (state) {
        case 1: return "open";
        case 2: return "half_open";
        default: return "closed";
    }
}

}  // namespace
struct Metrics::Impl {
    static constexpr std::size_t kOutcomes = 12;  // ProxySession::Outcome 取值数

    // atomic 不可移动，map 的值统一用 unique_ptr 包装
    struct UserModel {
        std::string labels;  // user="..",model=".."
        std::unique_ptr<std::atomic<std::uint64_t>[]> outcomes{new std::atomic<std::uint64_t>[kOutcomes]};
        std::atomic<std::int64_t> cost_micro_usd{0};

        UserModel() {
            for (std::size_t i = 0; i < kOutcomes; ++i) outcomes[i].store(0, std::memory_order_relaxed);
        }
    };

    struct SimpleCounter {
        std::string labels;
        std::atomic<std::uint64_t> value{0};
    };

    std::map<std::string, std::unique_ptr<UserModel>> user_models;  // key: user\x1fmodel
    std::map<std::string, std::unique_ptr<SimpleCounter>> tokens;   // user,model,upstream,type
    std::map<std::string, std::unique_ptr<SimpleCounter>> wasted;   // upstream,type
    std::map<std::string, std::unique_ptr<SimpleCounter>> unreported;  // upstream,field
    std::map<std::string, std::unique_ptr<SimpleCounter>> attempts;    // upstream,result
    std::map<std::string, std::unique_ptr<SimpleCounter>> cache_saved;  // model
    std::map<std::string, std::unique_ptr<SimpleCounter>> ratelimit;    // scope,dimension
    std::map<std::string, std::unique_ptr<SimpleCounter>> quota_rejections;  // user,period
    std::map<std::string, std::unique_ptr<SimpleCounter>> quota_truncations;  // user
    std::map<std::string, std::unique_ptr<std::atomic<std::uint64_t>>> redis_commands;  // command
    std::map<std::string, std::unique_ptr<std::atomic<std::uint64_t>>> redis_errors;    // kind
    std::map<std::string, std::unique_ptr<SimpleCounter>> cancellations;  // model,stage
    std::map<std::string, Hist> duration_hist;                            // model
    std::map<std::string, Hist> ttft_hist;                                // model
    std::map<std::string, Hist> ttfb_hist;                                // upstream
    std::unique_ptr<Hist> redis_latency;                                // Redis 命令延迟
    std::unique_ptr<Hist> ledger_commit;                                // 账本批量提交耗时
    std::atomic<std::uint64_t> ledger_write_errors{0};
    std::atomic<std::uint64_t> ledger_backlog{0};
    std::map<std::string, std::unique_ptr<std::atomic<int>>> circuit;     // upstream
    std::map<std::string, std::unique_ptr<SimpleCounter>> pool_idle;      // upstream（gauge，复用 counter 结构）

    std::atomic<std::uint64_t> cache_hits{0};
    std::atomic<std::uint64_t> cache_misses{0};
    std::atomic<std::uint64_t> cache_bytes{0};
    std::atomic<std::uint64_t> cache_entries{0};
    std::atomic<std::int64_t> active_streams{0};
    std::atomic<std::int64_t> backpressure_active{0};
    std::atomic<std::uint64_t> backpressure_pauses{0};

    std::vector<double> duration_bounds = {0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60, 120, 300};
    std::vector<double> ttft_bounds = {0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 30, 60};
    std::vector<double> ttfb_bounds = {0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60, 120, 300};

    UserModel& userModel(const std::string& user, const std::string& model) {
        std::string key = joinKey({user, model});
        auto it = user_models.find(key);
        if (it == user_models.end()) {
            // 未注册的组合（例如 model 不存在的 404）：计入 user="-" 的兜底序列，避免运行期加锁
            it = user_models.find(joinKey({"-", "-"}));
        }
        return *it->second;
    }

    SimpleCounter& counter(std::map<std::string, std::unique_ptr<SimpleCounter>>& m, const std::string& key,
                           std::string labels) {
        auto it = m.find(key);
        if (it == m.end()) it = m.emplace(key, std::make_unique<SimpleCounter>()).first;
        return *it->second;
    }
};

Metrics::Metrics(const Config& config) : impl_(std::make_unique<Impl>()) {
    auto addUserModel = [this](const std::string& user, const std::string& model) {
        auto um = std::make_unique<Impl::UserModel>();
        um->labels = renderLabels("user", user, "model", model);
        impl_->user_models.emplace(joinKey({user, model}), std::move(um));
    };
    for (const auto& [key, user] : config.users) {
        (void)key;
        for (const auto& [alias, model] : config.models) {
            (void)alias;
            addUserModel(user.name, model.alias);
        }
    }
    // 未注册组合的兜底（model 不存在的 404 等）
    addUserModel("-", "-");

    // 直方图按 model 维度（规格 §3.5 标签只有 model）
    for (const auto& [mkey, model] : config.models) {
        (void)mkey;
        impl_->duration_hist.emplace(model.alias, impl_->duration_bounds);
        impl_->ttft_hist.emplace(model.alias, impl_->ttft_bounds);
    }
    impl_->duration_hist.emplace("-", impl_->duration_bounds);
    impl_->ttft_hist.emplace("-", impl_->ttft_bounds);

    auto addCounter = [this](std::map<std::string, std::unique_ptr<Impl::SimpleCounter>>& m, const std::string& key,
                             std::string labels) {
        auto c = std::make_unique<Impl::SimpleCounter>();
        c->labels = std::move(labels);
        m.emplace(key, std::move(c));
    };

    for (const auto& [key, upstream] : config.upstreams) {
        (void)key;
        std::string un = upstream.name;
        for (auto field : {"prompt", "completion"}) {
            addCounter(impl_->unreported, joinKey({un, field}), renderLabels("upstream", un, "field", field));
        }
        for (auto type : kTokenTypes) {
            addCounter(impl_->wasted, joinKey({un, type}), renderLabels("upstream", un, "type", type));
        }
        for (auto result : kAttemptResults) {
            addCounter(impl_->attempts, joinKey({un, result}), renderLabels("upstream", un, "result", result));
        }
        impl_->circuit.emplace(un, std::make_unique<std::atomic<int>>(0));
        addCounter(impl_->pool_idle, joinKey({un}), renderLabels("upstream", un));
        impl_->ttfb_hist.emplace(un, impl_->ttfb_bounds);
    }

    // user × model × upstream × type
    for (const auto& [ukey, user] : config.users) {
        (void)ukey;
        for (const auto& [mkey, model] : config.models) {
            (void)mkey;
            for (const auto& [tkey, upstream] : config.upstreams) {
                (void)tkey;
                for (auto type : kTokenTypes) {
                    addCounter(impl_->tokens, joinKey({user.name, model.alias, upstream.name, type}),
                               renderLabels("user", user.name, "model", model.alias, "upstream", upstream.name,
                                            "type", type));
                }
            }
        }
    }

    for (const auto& [mkey, model] : config.models) {
        (void)mkey;
        addCounter(impl_->cache_saved, joinKey({model.alias}), renderLabels("model", model.alias));
        for (auto stage : {"acquiring", "awaiting", "streaming", "backoff"}) {
            addCounter(impl_->cancellations, joinKey({model.alias, stage}),
                       renderLabels("model", model.alias, "stage", stage));
        }
    }

    for (auto scope : {"user", "model"}) {
        for (auto dimension : {"requests", "tokens"}) {
            addCounter(impl_->ratelimit, joinKey({scope, dimension}),
                       renderLabels("scope", scope, "dimension", dimension));
        }
    }

    for (const auto& [ukey, user] : config.users) {
        (void)ukey;
        for (auto period : {"day", "month"}) {
            addCounter(impl_->quota_rejections, joinKey({user.name, period}),
                       renderLabels("user", user.name, "period", period));
        }
        addCounter(impl_->quota_truncations, joinKey({user.name}), renderLabels("user", user.name));
    }
    for (auto command : {"evalsha", "eval", "script", "get", "set", "other"}) {
        impl_->redis_commands.emplace(command, std::make_unique<std::atomic<std::uint64_t>>(0));
    }
    for (auto kind : {"timeout", "connection", "error_reply"}) {
        impl_->redis_errors.emplace(kind, std::make_unique<std::atomic<std::uint64_t>>(0));
    }
    impl_->redis_latency = std::make_unique<Hist>(
        std::vector<double>{0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5});
    impl_->ledger_commit = std::make_unique<Hist>(
        std::vector<double>{0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1, 5, 30});
}

Metrics::~Metrics() = default;

void Metrics::observeRequest(const RequestRecord& record) {
    auto& um = impl_->userModel(record.user, record.user.empty() ? "-" : record.model);
    std::size_t idx = static_cast<std::size_t>(ProxySession::Outcome::ok);
    // outcome 名 → 枚举序号
    for (int i = 0; i < static_cast<int>(Impl::kOutcomes); ++i) {
        if (record.outcome == toString(static_cast<ProxySession::Outcome>(i))) {
            idx = static_cast<std::size_t>(i);
            break;
        }
    }
    um.outcomes[idx].fetch_add(1, std::memory_order_relaxed);
    if (record.cost_usd) {
        um.cost_micro_usd.fetch_add(static_cast<std::int64_t>(*record.cost_usd * 1e6 + 0.5),
                                    std::memory_order_relaxed);
    }
    {
        std::string model = record.model.empty() ? "-" : record.model;
        if (auto h = impl_->duration_hist.find(model); h != impl_->duration_hist.end()) {
            h->second.observe(static_cast<double>(record.latency_ms) / 1e3);
        }
        if (record.ttft_ms) {
            if (auto h = impl_->ttft_hist.find(model); h != impl_->ttft_hist.end()) {
                h->second.observe(static_cast<double>(*record.ttft_ms) / 1e3);
            }
        }
    }

    // tokens_total / wasted_tokens_total：按尝试累加上游报告的值
    std::string model_key = record.model.empty() ? "-" : record.model;
    for (const auto& attempt : record.attempt_list) {
        const auto& u = attempt.usage;
        std::array<const std::optional<std::int64_t>*, 4> fields = {&u.prompt, &u.cached, &u.prompt_computed,
                                                                    &u.completion};
        bool wasted = attempt.result != "ok" || record.outcome == "cancelled" ||
                      record.outcome == "downstream_stalled" || record.outcome == "stream_interrupted" ||
                      record.outcome == "quota_truncated";
        for (std::size_t t = 0; t < kTokenTypes.size(); ++t) {
            if (!*fields[t]) continue;
            std::string tk = joinKey({record.user.empty() ? "-" : record.user, model_key, attempt.upstream,
                                      kTokenTypes[t]});
            if (auto series = impl_->tokens.find(tk); series != impl_->tokens.end()) {
                series->second->value.fetch_add(static_cast<std::uint64_t>(**fields[t]), std::memory_order_relaxed);
            }
            if (wasted) {
                std::string wk = joinKey({attempt.upstream, kTokenTypes[t]});
                if (auto series = impl_->wasted.find(wk); series != impl_->wasted.end()) {
                    series->second->value.fetch_add(static_cast<std::uint64_t>(**fields[t]),
                                                    std::memory_order_relaxed);
                }
            }
        }
        // 上游没有报告的字段：计 unreported 次数（rejected 尝试本来就是 0，不算）
        if (u.source != AttemptUsage::Source::rejected && u.source != AttemptUsage::Source::none) {
            for (auto field : {"prompt", "completion"}) {
                bool missing = field == std::string_view("prompt") ? !u.prompt.has_value() : !u.completion.has_value();
                if (!missing) continue;
                if (auto s = impl_->unreported.find(joinKey({attempt.upstream, field}));
                    s != impl_->unreported.end()) {
                    s->second->value.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
}

void Metrics::observeAttempt(const std::string& upstream, std::string_view result) {
    std::string k = joinKey({upstream, result});
    if (auto s = impl_->attempts.find(k); s != impl_->attempts.end())
        s->second->value.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observeUpstreamTtfb(const std::string& upstream, Duration ttfb) {
    if (auto h = impl_->ttfb_hist.find(upstream); h != impl_->ttfb_hist.end()) {
        h->second.observe(std::chrono::duration_cast<std::chrono::duration<double>>(ttfb).count());
    }
}

void Metrics::observeUnreported(const std::string& upstream, std::string_view field) {
    std::string k = joinKey({upstream, field});
    if (auto s = impl_->unreported.find(k); s != impl_->unreported.end())
        s->second->value.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observeCache(bool hit) {
    (hit ? impl_->cache_hits : impl_->cache_misses).fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observeCacheSavedCost(const std::string& model, std::int64_t micro_usd) {
    std::string k = joinKey({model});
    if (auto s = impl_->cache_saved.find(k); s != impl_->cache_saved.end())
        s->second->value.fetch_add(static_cast<std::uint64_t>(micro_usd), std::memory_order_relaxed);
}

void Metrics::setCacheGauges(std::uint64_t bytes, std::uint64_t entries) {
    impl_->cache_bytes.store(bytes, std::memory_order_relaxed);
    impl_->cache_entries.store(entries, std::memory_order_relaxed);
}

void Metrics::observeRatelimitRejection(std::string_view scope, std::string_view dimension) {
    std::string k = joinKey({scope, dimension});
    if (auto s = impl_->ratelimit.find(k); s != impl_->ratelimit.end())
        s->second->value.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observeQuotaRejection(const std::string& user, std::string_view period) {
    auto it = impl_->quota_rejections.find(joinKey({user, period}));
    if (it != impl_->quota_rejections.end()) it->second->value.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observeQuotaTruncation(const std::string& user) {
    auto it = impl_->quota_truncations.find(joinKey({user}));
    if (it != impl_->quota_truncations.end()) it->second->value.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observeRedisCommand(std::string_view command) {
    auto it = impl_->redis_commands.find(std::string(command));
    if (it != impl_->redis_commands.end()) it->second->fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observeRedisError(std::string_view kind) {
    auto it = impl_->redis_errors.find(std::string(kind));
    if (it != impl_->redis_errors.end()) it->second->fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observeRedisLatency(Duration latency) {
    impl_->redis_latency->observe(std::chrono::duration_cast<std::chrono::duration<double>>(latency).count());
}

void Metrics::observeLedgerCommit(Duration latency) {
    impl_->ledger_commit->observe(std::chrono::duration_cast<std::chrono::duration<double>>(latency).count());
}

void Metrics::observeLedgerWriteError() {
    impl_->ledger_write_errors.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::setLedgerBacklog(std::uint64_t backlog) {
    impl_->ledger_backlog.store(backlog, std::memory_order_relaxed);
}

void Metrics::observeCancellation(const std::string& model, std::string_view stage) {
    std::string k = joinKey({model, stage});
    if (auto s = impl_->cancellations.find(k); s != impl_->cancellations.end())
        s->second->value.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::setActiveStreams(std::int64_t value) {
    impl_->active_streams.store(value, std::memory_order_relaxed);
}

void Metrics::addActiveStreams(std::int64_t delta) {
    impl_->active_streams.fetch_add(delta, std::memory_order_relaxed);
}

void Metrics::addBackpressureActive(std::int64_t delta) {
    impl_->backpressure_active.fetch_add(delta, std::memory_order_relaxed);
}

void Metrics::observeBackpressurePause() {
    impl_->backpressure_pauses.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::setCircuitState(const std::string& upstream, int state) {
    if (auto s = impl_->circuit.find(upstream); s != impl_->circuit.end()) s->second->store(state);
}

void Metrics::setUpstreamIdle(const std::string& upstream, std::uint64_t idle) {
    std::string k = joinKey({upstream});
    if (auto s = impl_->pool_idle.find(k); s != impl_->pool_idle.end())
        s->second->value.store(idle, std::memory_order_relaxed);
}

std::string Metrics::renderPrometheus() const {
    std::string out;
    out.reserve(8192);
    auto line = [&out](std::string_view name, std::string_view type, std::string_view help) {
        out.append("# HELP ").append(name).push_back(' ');
        out.append(help).push_back('\n');
        out.append("# TYPE ").append(name).push_back(' ');
        out.append(type).push_back('\n');
    };

    line("gateway_requests_total", "counter", "Total requests by user, model, outcome");
    for (const auto& [key, um] : impl_->user_models) {
        (void)key;
        for (std::size_t i = 0; i < Impl::kOutcomes; ++i) {
            auto v = um->outcomes[i].load(std::memory_order_relaxed);
            if (v == 0) continue;
            out.append("gateway_requests_total{").append(um->labels).append(",outcome=\"");
            out.append(toString(static_cast<ProxySession::Outcome>(i))).append("\"} ");
            out.append(std::to_string(v)).push_back('\n');
        }
    }

    line("gateway_tokens_total", "counter", "Upstream-reported tokens by user, model, upstream, type");
    for (const auto& [key, c] : impl_->tokens) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_tokens_total{").append(c->labels).append("} ").append(std::to_string(v)).push_back('\n');
    }

    line("gateway_usage_unreported_total", "counter",
         "Attempts where upstream did not report a token field (never estimated)");
    for (const auto& [key, c] : impl_->unreported) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_usage_unreported_total{")
            .append(c->labels)
            .append("} ")
            .append(std::to_string(v))
            .push_back('\n');
    }

    line("gateway_wasted_tokens_total", "counter",
         "Tokens consumed by failed attempts and cancelled requests (upstream-reported)");
    for (const auto& [key, c] : impl_->wasted) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_wasted_tokens_total{")
            .append(c->labels)
            .append("} ")
            .append(std::to_string(v))
            .push_back('\n');
    }

    line("gateway_cost_usd_total", "counter", "Cumulative cost in USD by user, model");
    for (const auto& [key, um] : impl_->user_models) {
        (void)key;
        auto v = um->cost_micro_usd.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_cost_usd_total{").append(um->labels).append("} ");
        out.append(fmtDouble(microUsdToUsd(v))).push_back('\n');
    }

    auto renderHist = [&](std::string_view name, const std::map<std::string, Hist>& hist) {
        bool header_done = false;
        for (const auto& [model, h] : hist) {
            std::uint64_t total = h.count.load(std::memory_order_relaxed);
            if (total == 0) continue;
            if (!header_done) {
                line(name, "histogram", name == "gateway_request_duration_seconds"
                                            ? "Request duration by model"
                                            : "Time to first token by model");
                header_done = true;
            }
            std::string label_prefix = std::string("model=\"").append(model).append("\"");
            std::uint64_t cum = 0;
            for (std::size_t i = 0; i < h.bounds.size(); ++i) {
                cum += h.counts[i].load(std::memory_order_relaxed);
                out.append(name).append("_bucket{").append(label_prefix).append(",le=\"");
                out.append(fmtDouble(h.bounds[i])).append("\"} ").append(std::to_string(cum)).push_back('\n');
            }
            out.append(name).append("_bucket{").append(label_prefix).append(",le=\"+Inf\"} ");
            out.append(std::to_string(total)).push_back('\n');
            out.append(name).append("_sum{").append(label_prefix).append("} ");
            out.append(fmtDouble(static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1e6))
                .push_back('\n');
            out.append(name).append("_count{").append(label_prefix).append("} ");
            out.append(std::to_string(total)).push_back('\n');
        }
    };
    renderHist("gateway_request_duration_seconds", impl_->duration_hist);
    renderHist("gateway_ttft_seconds", impl_->ttft_hist);

    line("gateway_upstream_attempts_total", "counter", "Upstream attempts by upstream, result");
    for (const auto& [key, c] : impl_->attempts) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_upstream_attempts_total{")
            .append(c->labels)
            .append("} ")
            .append(std::to_string(v))
            .push_back('\n');
    }

    // upstream TTFB 直方图
    line("gateway_upstream_ttfb_seconds", "histogram", "Upstream time to first byte");
    for (const auto& [upstream, h] : impl_->ttfb_hist) {
        std::uint64_t total = h.count.load(std::memory_order_relaxed);
        if (total == 0) continue;
        std::string label_prefix = std::string("upstream=\"").append(upstream).append("\"");
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < h.bounds.size(); ++i) {
            cum += h.counts[i].load(std::memory_order_relaxed);
            out.append("gateway_upstream_ttfb_seconds_bucket{")
                .append(label_prefix)
                .append(",le=\"")
                .append(fmtDouble(h.bounds[i]))
                .append("\"} ")
                .append(std::to_string(cum))
                .push_back('\n');
        }
        out.append("gateway_upstream_ttfb_seconds_bucket{")
            .append(label_prefix)
            .append(",le=\"+Inf\"} ")
            .append(std::to_string(total))
            .push_back('\n');
        out.append("gateway_upstream_ttfb_seconds_sum{")
            .append(label_prefix)
            .append("} ")
            .append(fmtDouble(static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1e6))
            .push_back('\n');
        out.append("gateway_upstream_ttfb_seconds_count{")
            .append(label_prefix)
            .append("} ")
            .append(std::to_string(total))
            .push_back('\n');
    }

    line("gateway_upstream_circuit_state", "gauge", "Circuit breaker state: 0 closed / 1 open / 2 half_open");
    for (const auto& [upstream, state] : impl_->circuit) {
        out.append("gateway_upstream_circuit_state{upstream=\"")
            .append(upstream)
            .append("\"} ")
            .append(std::to_string(state->load(std::memory_order_relaxed)))
            .push_back('\n');
    }

    line("gateway_cache_requests_total", "counter", "Response cache lookups by result");
    {
        auto hits = impl_->cache_hits.load(std::memory_order_relaxed);
        auto misses = impl_->cache_misses.load(std::memory_order_relaxed);
        if (hits > 0)
            out.append("gateway_cache_requests_total{result=\"hit\"} ").append(std::to_string(hits)).push_back('\n');
        if (misses > 0)
            out.append("gateway_cache_requests_total{result=\"miss\"} ")
                .append(std::to_string(misses))
                .push_back('\n');
    }

    line("gateway_cache_saved_cost_usd_total", "counter", "Cost saved by cache hits in USD");
    for (const auto& [key, c] : impl_->cache_saved) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_cache_saved_cost_usd_total{")
            .append(c->labels)
            .append("} ")
            .append(fmtDouble(microUsdToUsd(static_cast<std::int64_t>(v))))
            .push_back('\n');
    }

    line("gateway_ratelimit_rejections_total", "counter", "Rate limit rejections by scope, dimension");
    for (const auto& [key, c] : impl_->ratelimit) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_ratelimit_rejections_total{")
            .append(c->labels)
            .append("} ")
            .append(std::to_string(v))
            .push_back('\n');
    }

    line("gateway_cancellations_total", "counter", "Client cancellations by model, stage");
    for (const auto& [key, c] : impl_->cancellations) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_cancellations_total{")
            .append(c->labels)
            .append("} ")
            .append(std::to_string(v))
            .push_back('\n');
    }

    line("gateway_quota_rejections_total", "counter", "Admission quota rejections by user, period");
    for (const auto& [key, c] : impl_->quota_rejections) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_quota_rejections_total{")
            .append(c->labels)
            .append("} ")
            .append(std::to_string(v))
            .push_back('\n');
    }

    line("gateway_quota_truncations_total", "counter", "Streaming truncations by user");
    for (const auto& [key, c] : impl_->quota_truncations) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_quota_truncations_total{")
            .append(c->labels)
            .append("} ")
            .append(std::to_string(v))
            .push_back('\n');
    }

    line("gateway_redis_commands_total", "counter", "Redis commands issued by command name");
    for (const auto& [command, v] : impl_->redis_commands) {
        auto n = v->load(std::memory_order_relaxed);
        if (n == 0) continue;
        out.append("gateway_redis_commands_total{command=\"").append(command).append("\"} ");
        out.append(std::to_string(n)).push_back('\n');
    }
    line("gateway_redis_errors_total", "counter", "Redis failures by kind");
    for (const auto& [kind, v] : impl_->redis_errors) {
        auto n = v->load(std::memory_order_relaxed);
        if (n == 0) continue;
        out.append("gateway_redis_errors_total{kind=\"").append(kind).append("\"} ");
        out.append(std::to_string(n)).push_back('\n');
    }

    line("gateway_ledger_backlog", "gauge", "Ledger rows waiting to be committed");
    out.append("gateway_ledger_backlog ")
        .append(std::to_string(impl_->ledger_backlog.load(std::memory_order_relaxed)))
        .push_back('\n');
    line("gateway_ledger_write_errors_total", "counter", "Ledger commit failures");
    out.append("gateway_ledger_write_errors_total ")
        .append(std::to_string(impl_->ledger_write_errors.load(std::memory_order_relaxed)))
        .push_back('\n');
    {
        const auto& h = *impl_->ledger_commit;
        std::uint64_t total = h.count.load(std::memory_order_relaxed);
        if (total > 0) {
            line("gateway_ledger_commit_seconds", "histogram", "Ledger batch commit latency");
            std::uint64_t cum = 0;
            for (std::size_t i = 0; i < h.bounds.size(); ++i) {
                cum += h.counts[i].load(std::memory_order_relaxed);
                out.append("gateway_ledger_commit_seconds_bucket{le=\"")
                    .append(fmtDouble(h.bounds[i]))
                    .append("\"} ")
                    .append(std::to_string(cum))
                    .push_back('\n');
            }
            out.append("gateway_ledger_commit_seconds_bucket{le=\"+Inf\"} ")
                .append(std::to_string(total))
                .push_back('\n');
            out.append("gateway_ledger_commit_seconds_sum ")
                .append(fmtDouble(static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1e6))
                .push_back('\n');
            out.append("gateway_ledger_commit_seconds_count ")
                .append(std::to_string(total))
                .push_back('\n');
        }
    }
    {
        const auto& h = *impl_->redis_latency;
        std::uint64_t total = h.count.load(std::memory_order_relaxed);
        if (total > 0) {
            line("gateway_redis_latency_seconds", "histogram", "Redis command latency");
            std::uint64_t cum = 0;
            for (std::size_t i = 0; i < h.bounds.size(); ++i) {
                cum += h.counts[i].load(std::memory_order_relaxed);
                out.append("gateway_redis_latency_seconds_bucket{le=\"")
                    .append(fmtDouble(h.bounds[i]))
                    .append("\"} ")
                    .append(std::to_string(cum))
                    .push_back('\n');
            }
            out.append("gateway_redis_latency_seconds_bucket{le=\"+Inf\"} ")
                .append(std::to_string(total))
                .push_back('\n');
            out.append("gateway_redis_latency_seconds_sum ")
                .append(fmtDouble(static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1e6))
                .push_back('\n');
            out.append("gateway_redis_latency_seconds_count ")
                .append(std::to_string(total))
                .push_back('\n');
        }
    }

    line("gateway_cache_bytes", "gauge", "Response cache bytes in use");
    out.append("gateway_cache_bytes ")
        .append(std::to_string(impl_->cache_bytes.load(std::memory_order_relaxed)))
        .push_back('\n');
    line("gateway_cache_entries", "gauge", "Response cache entries");
    out.append("gateway_cache_entries ")
        .append(std::to_string(impl_->cache_entries.load(std::memory_order_relaxed)))
        .push_back('\n');

    line("gateway_active_streams", "gauge", "Currently streaming proxy sessions");
    out.append("gateway_active_streams ")
        .append(std::to_string(impl_->active_streams.load(std::memory_order_relaxed)))
        .push_back('\n');

    line("gateway_backpressure_active", "gauge", "Streams currently paused by backpressure");
    out.append("gateway_backpressure_active ")
        .append(std::to_string(impl_->backpressure_active.load(std::memory_order_relaxed)))
        .push_back('\n');
    line("gateway_backpressure_pauses_total", "counter", "Backpressure pause events");
    out.append("gateway_backpressure_pauses_total ")
        .append(std::to_string(impl_->backpressure_pauses.load(std::memory_order_relaxed)))
        .push_back('\n');

    line("gateway_upstream_pool_idle", "gauge", "Idle connections in the per-loop upstream pools");
    for (const auto& [key, c] : impl_->pool_idle) {
        (void)key;
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        out.append("gateway_upstream_pool_idle{")
            .append(c->labels)
            .append("} ")
            .append(std::to_string(v))
            .push_back('\n');
    }

    return out;
}

std::string Metrics::renderStats() const {
    auto msOf = [](double seconds) { return static_cast<std::int64_t>(seconds * 1e3); };
    // joinKey 的 \x1f 分隔符只用于内部键，输出时换成可读分隔
    auto displayKey = [](const std::string& key) {
        std::string out = key;
        std::replace(out.begin(), out.end(), '\x1f', '|');
        return out;
    };
    nlohmann::json out;

    out["active_streams"] = impl_->active_streams.load(std::memory_order_relaxed);
    out["backpressure"] = {{"active", impl_->backpressure_active.load(std::memory_order_relaxed)},
                           {"pauses_total", impl_->backpressure_pauses.load(std::memory_order_relaxed)}};

    nlohmann::json cache = {{"hits", impl_->cache_hits.load(std::memory_order_relaxed)},
                            {"misses", impl_->cache_misses.load(std::memory_order_relaxed)},
                            {"bytes", impl_->cache_bytes.load(std::memory_order_relaxed)},
                            {"entries", impl_->cache_entries.load(std::memory_order_relaxed)}};
    nlohmann::json saved = nlohmann::json::object();
    std::int64_t saved_total = 0;
    for (const auto& [key, c] : impl_->cache_saved) {
        auto v = static_cast<std::int64_t>(c->value.load(std::memory_order_relaxed));
        saved_total += v;
        saved[key] = microUsdToUsd(v);
    }
    cache["saved_cost_usd_total"] = microUsdToUsd(saved_total);
    cache["saved_cost_usd_by_model"] = std::move(saved);
    out["cache"] = std::move(cache);

    nlohmann::json users = nlohmann::json::object();
    nlohmann::json models = nlohmann::json::object();
    for (const auto& [key, um] : impl_->user_models) {
        std::uint64_t total = 0;
        nlohmann::json outcomes = nlohmann::json::object();
        bool any = false;
        for (std::size_t i = 0; i < Impl::kOutcomes; ++i) {
            auto v = um->outcomes[i].load(std::memory_order_relaxed);
            if (v == 0) continue;
            outcomes[toString(static_cast<ProxySession::Outcome>(i))] = v;
            total += v;
            any = true;
        }
        if (!any) continue;
        std::size_t sep = key.find('\x1f');
        std::string user = key.substr(0, sep);
        std::string model = key.substr(sep + 1);
        nlohmann::json entry = {{"requests", total},
                                {"outcomes", std::move(outcomes)},
                                {"cost_usd", microUsdToUsd(um->cost_micro_usd.load(std::memory_order_relaxed))}};
        if (user == "-") {
            users["-"] = entry;
        } else {
            // tokens / 直方图按 (user, model) 键聚合到 model 视图
            models[model] = entry;
        }
    }
    out["users"] = std::move(users);

    // model 视图补齐 tokens 与分位数（tokens 键是 user\x1fmodel\x1fupstream\x1ftype）
    std::map<std::string, std::map<std::string, std::uint64_t>> tokens_by_model;
    for (const auto& [key, c] : impl_->tokens) {
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        // user \x1f model \x1f upstream \x1f type
        std::size_t p1 = key.find('\x1f');
        std::size_t p2 = key.find('\x1f', p1 + 1);
        std::size_t p3 = key.find('\x1f', p2 + 1);
        std::string model = key.substr(p1 + 1, p2 - p1 - 1);
        std::string type = key.substr(p3 + 1);
        tokens_by_model[model][type] += v;
    }
    nlohmann::json model_tokens = nlohmann::json::object();
    for (const auto& [model, types] : tokens_by_model) {
        nlohmann::json t = nlohmann::json::object();
        for (const auto& [type, v] : types) t[type] = v;
        model_tokens[model] = std::move(t);
    }
    for (auto& [model, entry] : models.items()) {
        if (model_tokens.contains(model)) entry["tokens"] = model_tokens[model];
        std::int64_t latency_p50 = 0, latency_p95 = 0, ttft_p50 = 0, ttft_p95 = 0;
        if (auto h = impl_->duration_hist.find(model); h != impl_->duration_hist.end() &&
                                                         h->second.count.load(std::memory_order_relaxed) > 0) {
            latency_p50 = msOf(h->second.quantile(0.50));
            latency_p95 = msOf(h->second.quantile(0.95));
        }
        if (auto h = impl_->ttft_hist.find(model); h != impl_->ttft_hist.end() &&
                                                    h->second.count.load(std::memory_order_relaxed) > 0) {
            ttft_p50 = msOf(h->second.quantile(0.50));
            ttft_p95 = msOf(h->second.quantile(0.95));
        }
        entry["latency_p50_ms"] = latency_p50;
        entry["latency_p95_ms"] = latency_p95;
        entry["ttft_p50_ms"] = ttft_p50;
        entry["ttft_p95_ms"] = ttft_p95;
    }
    out["models"] = std::move(models);

    nlohmann::json upstreams = nlohmann::json::object();
    for (const auto& [upstream, state] : impl_->circuit) {
        nlohmann::json u;
        nlohmann::json att = nlohmann::json::object();
        for (auto result : kAttemptResults) {
            std::string k = joinKey({upstream, result});
            if (auto s = impl_->attempts.find(k); s != impl_->attempts.end()) {
                auto v = s->second->value.load(std::memory_order_relaxed);
                if (v > 0) att[result] = v;
            }
        }
        u["attempts"] = std::move(att);
        int st = state->load(std::memory_order_relaxed);
        u["circuit_state"] = st;
        u["circuit_state_name"] = circuitStateName(st);
        if (auto s = impl_->pool_idle.find(joinKey({upstream})); s != impl_->pool_idle.end()) {
            u["idle_connections"] = s->second->value.load(std::memory_order_relaxed);
        }
        if (auto h = impl_->ttfb_hist.find(upstream); h != impl_->ttfb_hist.end() &&
                                                        h->second.count.load(std::memory_order_relaxed) > 0) {
            u["ttfb_p50_ms"] = msOf(h->second.quantile(0.50));
            u["ttfb_p95_ms"] = msOf(h->second.quantile(0.95));
        }
        upstreams[upstream] = std::move(u);
    }
    out["upstreams"] = std::move(upstreams);

    nlohmann::json unreported = nlohmann::json::object();
    for (const auto& [key, c] : impl_->unreported) {
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        unreported[displayKey(key)] = v;
    }
    out["usage_unreported_total"] = std::move(unreported);

    nlohmann::json rejections = nlohmann::json::object();
    for (const auto& [key, c] : impl_->ratelimit) {
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        rejections[displayKey(key)] = v;
    }
    out["ratelimit_rejections_total"] = std::move(rejections);

    nlohmann::json cancellations = nlohmann::json::object();
    for (const auto& [key, c] : impl_->cancellations) {
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        cancellations[displayKey(key)] = v;
    }
    out["cancellations_total"] = std::move(cancellations);

    nlohmann::json wasted = nlohmann::json::object();
    for (const auto& [key, c] : impl_->wasted) {
        auto v = c->value.load(std::memory_order_relaxed);
        if (v == 0) continue;
        wasted[displayKey(key)] = v;
    }
    out["wasted_tokens_total"] = std::move(wasted);

    return out.dump();
}

}  // namespace gw::gateway
