#include "gateway/proxy_session.hpp"

#include "base/log.hpp"
#include "base/random.hpp"
#include "gateway/access_log.hpp"
#include "gateway/admin.hpp"
#include "gateway/ledger.hpp"
#include "gateway/metrics.hpp"
#include "gateway/pricing.hpp"
#include "gateway/usage_meter.hpp"
#include "http/response_writer.hpp"
#include "proto/errors.hpp"
#include "proto/token_estimate.hpp"

namespace gw::gateway {

const char* toString(ProxySession::Stage stage) {
    switch (stage) {
        case ProxySession::Stage::admitting: return "admitting";
        case ProxySession::Stage::acquiring: return "acquiring";
        case ProxySession::Stage::await_head: return "await_head";
        case ProxySession::Stage::await_first_event: return "await_first_event";
        case ProxySession::Stage::streaming: return "streaming";
        case ProxySession::Stage::backoff: return "backoff";
        case ProxySession::Stage::done: return "done";
    }
    return "?";
}

const char* toString(ProxySession::Outcome outcome) {
    switch (outcome) {
        case ProxySession::Outcome::ok: return "ok";
        case ProxySession::Outcome::cache_hit: return "cache_hit";
        case ProxySession::Outcome::upstream_error: return "upstream_error";
        case ProxySession::Outcome::rate_limited: return "rate_limited";
        case ProxySession::Outcome::bad_request: return "bad_request";
        case ProxySession::Outcome::unauthorized: return "unauthorized";
        case ProxySession::Outcome::cancelled: return "cancelled";
        case ProxySession::Outcome::downstream_stalled: return "downstream_stalled";
        case ProxySession::Outcome::stream_interrupted: return "stream_interrupted";
        case ProxySession::Outcome::unavailable: return "unavailable";
        case ProxySession::Outcome::quota_exceeded: return "quota_exceeded";
        case ProxySession::Outcome::quota_truncated: return "quota_truncated";
    }
    return "?";
}

ProxySession::ProxySession(GatewayContext& context, http::HttpServer& server, net::TcpConnectionPtr conn,
                           Ingress ingress)
    : context_(context), server_(server), downstream_(conn), ingress_(std::move(ingress)) {
    context_.active_sessions.fetch_add(1);
    wall_ts_ = wallMillis();
}

ProxySession::~ProxySession() {
    context_.active_sessions.fetch_sub(1);
}

net::TcpConnectionPtr ProxySession::downstream() {
    return downstream_.lock();
}

void ProxySession::start() {
    admit();
}

void ProxySession::admit() {
    // 准入顺序见 proxy-session.md §1：1 鉴权（Router）→ 2 解析 → 3 路由 → 3a 账本 → 3b 配额 → 4 限流 → 7
    try {
        request_json_ = nlohmann::json::parse(ingress_.http.body);
    } catch (const std::exception& e) {
        writeErrorResponse(400,
                           proto::errorBody(ingress_.protocol, std::string("invalid JSON: ") + e.what(),
                                            "invalid_request_error"),
                           true);
        finish(Outcome::bad_request);
        return;
    }
    count_tokens_ = ingress_.http.path == "/v1/messages/count_tokens";
    auto meta = ingress_.protocol == proto::Protocol::openai ? proto::parseOpenAiRequest(request_json_)
                                                           : proto::parseAnthropicRequest(request_json_);
    if (!meta) {
        writeErrorResponse(400, proto::errorBody(ingress_.protocol, meta.error(), "invalid_request_error"), true);
        finish(Outcome::bad_request);
        return;
    }
    if (ingress_.protocol == proto::Protocol::anthropic && !count_tokens_ &&
        (!meta->max_tokens || *meta->max_tokens <= 0)) {
        writeErrorResponse(400, proto::errorBody(ingress_.protocol, "max_tokens must be a positive integer", "invalid_request_error"), false);
        finish(Outcome::bad_request); return;
    }
    meta_ = *meta;
    if (count_tokens_) { meta_.stream = false; request_json_["stream"] = false; }

    route_ = context_.config->findModel(meta_.model);
    if (route_ == nullptr) {
        writeErrorResponse(404,
                           proto::errorBody(ingress_.protocol, "model not found: " + meta_.model,
                                            "invalid_request_error", "model_not_found"),
                           true);
        finish(Outcome::bad_request);
        return;
    }
    if (count_tokens_) {
        bool supported = false;
        for (const auto& name : route_->targets) if (context_.config->findUpstream(name)->count_tokens) supported = true;
        if (!supported) {
            writeErrorResponse(501, proto::errorBody(ingress_.protocol, "no upstream supports count_tokens", "not_implemented_error"), false);
            finish(Outcome::bad_request); return;
        }
    }
    checkLedgerAndQuota();
}

void ProxySession::checkLedgerAndQuota() {
    // 3a 账本健康：计费不可用就拒绝服务（fail closed）
    if (context_.ledger_writer != nullptr && !context_.ledger_writer->healthy()) {
        writeErrorResponse(503,
                           proto::errorBody(ingress_.protocol, "usage ledger unavailable",
                                            ingress_.protocol == proto::Protocol::anthropic ? "overloaded_error"
                                                                                            : "service_unavailable"),
                           true);
        finish(Outcome::unavailable);
        return;
    }
    // 3b 配额：仅对配置了 quota 的 key；其他 key 直接跳过、不进入 Admitting
    store::RedisClient* redis = redisClient();
    if (ingress_.user == nullptr || !ingress_.user->quota || context_.quota_manager == nullptr || redis == nullptr) {
        proceedAdmission();
        return;
    }
    stage_ = Stage::admitting;
    int64_t now = wallMillis();
    int64_t day = quotaDayOf(now, context_.config->quota.utc_offset_minutes);
    int64_t month = quotaMonthOf(now, context_.config->quota.utc_offset_minutes);
    std::weak_ptr<ProxySession> weak = shared_from_this();
    context_.quota_manager->get(
        *redis, ingress_.user->name, day, month, [weak](std::expected<QuotaUsage, store::RedisError> result) {
            if (auto self = weak.lock()) {
                if (result) {
                    self->onQuotaChecked(result->used_day, result->used_month);
                } else {
                    self->onQuotaFailed(result.error().message);
                }
            }
        });
}

void ProxySession::onQuotaChecked(std::int64_t used_day, std::int64_t used_month) {
    if (finished_) return;
    const auto& q = *ingress_.user->quota;
    const auto& qc = context_.config->quota;
    std::optional<std::int64_t> remaining;  // nullopt = 不限
    int64_t now = wallMillis();
    if (q.daily_tokens > 0) {
        remaining = q.daily_tokens - used_day;
    }
    if (q.monthly_tokens > 0) {
        int64_t month_remaining = q.monthly_tokens - used_month;
        remaining = remaining ? std::min(*remaining, month_remaining) : month_remaining;
    }
    if (remaining.has_value() && *remaining <= 0) {
        // 准入超额：429 insufficient_quota，Retry-After 为到下个周期开始的秒数
        int64_t retry = secondsToNextLocalMidnight(now, qc.utc_offset_minutes);
        if (q.monthly_tokens > 0 && used_month >= q.monthly_tokens) {
            retry = 30 * 86400;  // 月配额用尽：近似到下月初；客户端会经 /v1/usage 查询实际重置时间
        }
        http::Headers headers;
        headers.emplace_back("retry-after", std::to_string(std::max<std::int64_t>(retry, 1)));
        headers.emplace_back("x-gateway-quota-remaining-tokens", "0");
        if (context_.metrics) {
            const char* period = (q.monthly_tokens > 0 && used_month >= q.monthly_tokens) ? "month" : "day";
            context_.metrics->observeQuotaRejection(ingress_.user->name, period);
        }
        writeErrorResponse(429, proto::errorBody(ingress_.protocol, "token quota exceeded",
                                                 ingress_.protocol == proto::Protocol::anthropic
                                                     ? "rate_limit_error"
                                                     : "insufficient_quota",
                                                 "insufficient_quota"),
                           false, headers);
        finish(Outcome::quota_exceeded);
        return;
    }
    quota_view_.emplace();
    quota_view_->remaining = remaining;  // nullopt = 不限
    proceedAdmission();
}

void ProxySession::onQuotaFailed(const std::string& message) {
    if (finished_) return;
    if (context_.config->quota.on_redis_failure == "reject") {
        writeErrorResponse(503,
                           proto::errorBody(ingress_.protocol, "quota backend unavailable",
                                            ingress_.protocol == proto::Protocol::anthropic ? "overloaded_error"
                                                                                            : "service_unavailable"),
                           true);
        finish(Outcome::unavailable);
        return;
    }
    SPDLOG_WARN("quota check failed ({}), allowing request {} (on_redis_failure=allow)", message,
                ingress_.request_id);
    quota_view_.emplace();  // remaining = nullopt：不截断，照常尝试上报
    proceedAdmission();
}

void ProxySession::proceedAdmission() {
    // 4 限流
    if (!acquireRateTicket()) return;
    // 5 缓存（命中即回放结束）
    if (checkCache()) return;
    // 6 尝试计划：跳过熔断打开的目标
    if (!selectTarget()) {
        writeErrorResponse(503,
                           proto::errorBody(ingress_.protocol, "all upstreams unavailable (circuit open)",
                                            "upstream_unavailable"),
                           true);
        finish(Outcome::unavailable);
        return;
    }
    startPrecommitTimer();
    startAttempt();
}

bool ProxySession::cacheableRequest() const {
    ResponseCache* cache = context_.response_cache;
    if (count_tokens_ || cache == nullptr || !cache->enabled()) return false;
    bool bypass = false;
    bool force = false;
    if (auto header = ingress_.http.header("x-gateway-cache")) {
        if (*header == "bypass") bypass = true;
        if (*header == "force") force = true;
    }
    if (bypass) return false;
    if (!force) {
        // 请求显式 temperature == 0 才缓存（Qwen3.6 服务端默认 0.6）
        if (!meta_.temperature.has_value() || *meta_.temperature != 0.0) return false;
    }
    if (meta_.n_greater_than_one) return false;
    return true;
}

bool ProxySession::checkCache() {
    if (!cacheableRequest()) return false;
    const std::string protocol = ingress_.protocol == proto::Protocol::anthropic ? "anthropic" : "openai";
    cache_key_ = ResponseCache::makeKey(protocol, meta_.stream, meta_.client_wants_usage, meta_.model, request_json_);
    if (auto entry = context_.response_cache->get(cache_key_)) {
        cache_hit_ = true;
        cache_entry_ = std::move(*entry);
        if (context_.metrics) context_.metrics->observeCache(true);
        SPDLOG_DEBUG("proxy: cache HIT request_id={} key={}", ingress_.request_id, ingress_.request_id);
        replayCache();
        return true;
    }
    cache_miss_ = true;
    if (context_.metrics) context_.metrics->observeCache(false);
    return false;
}

void ProxySession::replayCache() {
    auto conn = downstream();
    if (!conn || !conn->connected()) {
        finish(Outcome::cache_hit);
        return;
    }
    http::Headers headers;
    headers.emplace_back("x-gateway-cache", "HIT");
    headers.emplace_back("x-gateway-upstream", cache_entry_->upstream);
    headers.emplace_back("x-gateway-attempts", "0");
    for (const auto& h : rateHeaders()) headers.push_back(h);
    http::ResponseWriter writer(downstream_, ingress_.request_id, true);
    http_status_ = 200;
    if (cache_entry_->stream) {
        writer.writeStreamHead(200, headers);
        committed_ = true;
        ttft_ = nowSteady() - ingress_.received_at;
        if (context_.metrics) context_.metrics->addActiveStreams(1);
        writer.writeChunk(cache_entry_->body);
        writer.endStream();
    } else {
        writer.writeFull(200, headers,
                         cache_entry_->content_type.empty() ? "application/json" : cache_entry_->content_type,
                         cache_entry_->body);
    }
    finish(Outcome::cache_hit);
}

void ProxySession::stageCacheBytes(std::string_view framed) {
    if (cache_key_.empty() || cache_hit_ || !meta_.stream) return;
    if (cache_overflow_ || context_.response_cache == nullptr) return;
    if (cache_staging_.size() + framed.size() > context_.response_cache->maxEntryBytes()) {
        cache_overflow_ = true;
        cache_staging_.clear();
        return;
    }
    cache_staging_.append(framed);
}

void ProxySession::writeCacheEntry() {
    if (cache_key_.empty() || cache_hit_ || context_.response_cache == nullptr) return;
    if (!context_.response_cache->enabled() || outcome_ != Outcome::ok) return;
    if (meter_ == nullptr || meter_->value().source != AttemptUsage::Source::upstream_final) return;
    std::string finish;
    if (meta_.stream) {
        if (!decoder_) return;
        finish = proto::encodeFinish(proto::decodeFinish(decoder_->finishReason(), upstream_protocol_), proto::Protocol::openai);
    } else {
        try {
            auto body = nlohmann::json::parse(nonstream_body_);
            if (body.contains("choices") && body["choices"].is_array() && !body["choices"].empty()) {
                finish = body["choices"][0].value("finish_reason", "");
            } else if (body.contains("stop_reason")) {
                finish = proto::encodeFinish(proto::decodeFinish(body.value("stop_reason", ""), proto::Protocol::anthropic), proto::Protocol::openai);
            }
        } catch (...) {
            return;
        }
    }
    if (finish != "stop" && finish != "length" && finish != "tool_calls") return;

    auto entry = std::make_shared<ResponseCache::Entry>();
    entry->key = cache_key_;
    entry->stream = meta_.stream;
    if (meta_.stream) {
        if (cache_overflow_ || cache_staging_.empty()) return;
        entry->body = cache_staging_;
        entry->content_type = "text/event-stream";
    } else {
        if (nonstream_body_.empty()) return;
        entry->body = nonstream_body_;
        entry->content_type = "application/json";
    }
    entry->usage = meter_->value();
    entry->upstream = target_ ? target_->name : "";
    entry->finish_reason = finish;
    context_.response_cache->put(cache_key_, std::move(entry));
    auto [bytes, entries] = context_.response_cache->stats();
    if (context_.metrics) context_.metrics->setCacheGauges(bytes, entries);
    SPDLOG_DEBUG("proxy: cache PUT request_id={} stream={} bytes={}", ingress_.request_id, meta_.stream,
                 meta_.stream ? cache_staging_.size() : nonstream_body_.size());
}

bool ProxySession::selectTarget() {
    while (target_index_ < route_->targets.size()) {
        const UpstreamConfig* candidate = context_.config->findUpstream(route_->targets[target_index_]);
        if (count_tokens_ && candidate && !candidate->count_tokens) { ++target_index_; continue; }
        if (candidate == nullptr) {
            ++target_index_;
            attempt_in_target_ = 1;
            continue;
        }
        CircuitBreaker* breaker = context_.breakers ? context_.breakers->get(candidate->name) : nullptr;
        if (breaker != nullptr && !breaker->allowRequest()) {
            SPDLOG_DEBUG("proxy: skip upstream {} (circuit {}) request_id={}", candidate->name,
                         CircuitBreaker::stateName(breaker->state()), ingress_.request_id);
            ++target_index_;
            attempt_in_target_ = 1;
            continue;
        }
        target_ = candidate;
        return true;
    }
    return false;
}

bool ProxySession::acquireRateTicket() {
    if (context_.rate_limiter == nullptr || ingress_.user == nullptr) return true;
    // 预留量 = token_estimate(请求) + max_tokens（缺省取模型别名的 default_max_tokens，默认 4096）。
    // 这是全项目唯一使用估算的地方：预留不是消耗，不进记录、指标和成本
    double estimate = static_cast<double>(proto::estimateJsonRequestTokens(request_json_));
    double max_tokens = meta_.max_tokens ? static_cast<double>(*meta_.max_tokens)
                                         : static_cast<double>(route_->default_max_tokens);
    auto result = context_.rate_limiter->tryAcquire(*ingress_.user, *route_, estimate + max_tokens);
    if (!result) {
        const auto& rej = result.error();
        auto retry_after = std::chrono::ceil<std::chrono::seconds>(rej.retry_after);
        if (retry_after < std::chrono::seconds(1)) retry_after = std::chrono::seconds(1);
        http::Headers headers;
        headers.emplace_back("retry-after", std::to_string(retry_after.count()));
        headers.emplace_back("x-gateway-ratelimit-scope", rej.scope);
        headers.emplace_back("x-gateway-ratelimit-dimension", rej.dimension);
        if (context_.metrics) {
            context_.metrics->observeRatelimitRejection(rej.scope, rej.dimension);
        }
        writeErrorResponse(429, proto::errorBody(ingress_.protocol, "rate limit exceeded, retry later",
                                                 "rate_limit_error", "rate_limited"),
                           false, headers);
        finish(Outcome::rate_limited);
        return false;
    }
    rate_ticket_ = *result;
    return true;
}

http::Headers ProxySession::rateHeaders() const {
    http::Headers headers;
    if (!rate_ticket_) return headers;
    auto fmt = [](std::optional<double> v) -> std::optional<std::string> {
        if (!v) return std::nullopt;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.0f", *v);
        return std::string(buf);
    };
    if (auto v = fmt(rate_ticket_->limit_requests))
        headers.emplace_back("x-ratelimit-limit-requests", *v);
    if (auto v = fmt(rate_ticket_->remaining_requests))
        headers.emplace_back("x-ratelimit-remaining-requests", *v);
    if (auto v = fmt(rate_ticket_->limit_tokens)) headers.emplace_back("x-ratelimit-limit-tokens", *v);
    if (auto v = fmt(rate_ticket_->remaining_tokens))
        headers.emplace_back("x-ratelimit-remaining-tokens", *v);
    return headers;
}

void ProxySession::startAttempt(bool allow_idle, bool resend) {
    if (target_ == nullptr) {
        writeErrorResponse(503,
                           proto::errorBody(ingress_.protocol, "upstream not configured", "upstream_unavailable"),
                           true);
        finish(Outcome::unavailable);
        return;
    }
    upstream_protocol_ = target_->kind == "anthropic" ? proto::Protocol::anthropic : proto::Protocol::openai;
    upstream_request_ = request_json_;
    if (!count_tokens_ && ingress_.protocol != upstream_protocol_) {
        auto neutral = proto::decodeRequest(request_json_, ingress_.protocol);
        if (!neutral) {
            writeErrorResponse(400, proto::errorBody(ingress_.protocol, neutral.error(), "invalid_request_error"), false);
            finish(Outcome::bad_request); return;
        }
        neutral->model = target_->model;
        try {
            upstream_request_ = proto::encodeRequest(*neutral, upstream_protocol_, target_->default_max_tokens);
        } catch (const std::exception& e) {
            writeErrorResponse(400, proto::errorBody(ingress_.protocol, e.what(), "invalid_request_error"), false);
            finish(Outcome::bad_request); return;
        }
    }
    key_ = http::UpstreamKey{target_->url.host, target_->url.port};
    if (!resend) {
        // 每次尝试独立计量与解析；请求级状态（committed_/ttft_/背压）跨尝试保留
        cancelTimer(ttft_timer_);
        cancelTimer(idle_timer_);
        cancelTimer(total_timer_);
        ++attempts_;
        ++attempt_epoch_;
        attempt_recorded_ = false;
        meter_ = std::make_unique<UsageMeter>(target_->metering);
        meter_finalized_ = false;
        stream_observer_.reset();
        decoder_.reset();
        encoder_.reset();
        anthropic_state_ = proto::AnthropicStreamState{};
        sse_parser_.reset();
        cache_staging_.clear();
        nonstream_body_.clear();
        upstream_status_ = 0;
        upstream_headers_.clear();
        stream_completed_ = false;
        endstream_sent_ = false;
        first_progress_seen_ = false;
        reuse_retried_ = false;
        upstream_reusable_ = false;
        last_connect_ms_.reset();
        last_conn_reused_.reset();
        exchange_.reset();
    }

    auto conn = downstream();
    if (!conn) return;
    net::EventLoop* loop = conn->getLoop();
    auto* services_ptr = std::any_cast<std::shared_ptr<gateway::IoLoopServices>>(&loop->context());
    if (services_ptr == nullptr || (*services_ptr)->pool == nullptr) {
        writeErrorResponse(503,
                           proto::errorBody(ingress_.protocol, "upstream pool not initialized", "upstream_error"),
                           true);
        finish(Outcome::unavailable);
        return;
    }
    auto pool = (*services_ptr)->pool;

    stage_ = Stage::acquiring;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    acquire_handle_ = pool->acquire(        key_, target_->timeouts.connect,
        [weak](std::expected<http::Lease, http::AcquireError> result) {
            if (auto self = weak.lock()) self->onAcquire(std::move(result));
        },
        allow_idle);
}

void ProxySession::onAcquire(std::expected<http::Lease, http::AcquireError> result) {
    if (finished_) return;
    if (!result) {
        if (cancelled_ || result.error().kind == http::AcquireError::Kind::cancelled) {
            finish(Outcome::cancelled);
            return;
        }
        TransportFailure kind = TransportFailure::connect_refused;
        switch (result.error().kind) {
            case http::AcquireError::Kind::resolve: kind = TransportFailure::dns; break;
            case http::AcquireError::Kind::connect_refused: kind = TransportFailure::connect_refused; break;
            case http::AcquireError::Kind::connect_timeout: kind = TransportFailure::connect_timeout; break;
            case http::AcquireError::Kind::cancelled: kind = TransportFailure::io_error; break;
        }
        attemptFailure(classifyTransport(kind), 0, "upstream connect failed: " + result.error().message, true,
                       std::nullopt);
        return;
    }

    // final 计量的上游流中没有计数：准入时把发给上游的 max_tokens 限制为 remaining（来自 Redis，不是估算）
    nlohmann::json upstream_json = upstream_request_;
    if (target_->metering == "final" && quota_view_ && quota_view_->remaining) {
        int cap = meta_.max_tokens.value_or(route_ ? route_->default_max_tokens : 4096);
        if (quota_view_->remaining.has_value()) {
            cap = std::min<int64_t>(cap, std::max<int64_t>(*quota_view_->remaining, 1));
        }
        upstream_json["max_tokens"] = cap;
    }
    if (upstream_protocol_ == proto::Protocol::openai && !count_tokens_) {
        upstream_json = proto::rewriteOpenAiRequest(upstream_json, target_->model, target_->metering, target_->extra_body, meta_.stream);
    } else {
        upstream_json["model"] = target_->model;
        if (!count_tokens_ && !upstream_json.contains("max_tokens")) upstream_json["max_tokens"] = target_->default_max_tokens;
        if (target_->extra_body.is_object()) for (const auto& [key, value] : target_->extra_body.items())
            if (!upstream_json.contains(key)) upstream_json[key] = value;
    }
    std::string upstream_body = upstream_json.dump();

    http::UpstreamRequest request;
    request.method = "POST";
    request.path = (target_->url.path_prefix.empty() ? std::string() : target_->url.path_prefix) + (count_tokens_ ? "/messages/count_tokens" : upstream_protocol_ == proto::Protocol::anthropic ? "/messages" : "/chat/completions");
    request.headers.emplace_back("content-type", "application/json");
    request.headers.emplace_back("accept-encoding", "identity");
    if (meta_.stream) request.headers.emplace_back("accept", "text/event-stream");
    if (upstream_protocol_ == proto::Protocol::anthropic || count_tokens_) {
        request.headers.emplace_back("anthropic-version", std::string(ingress_.http.header("anthropic-version").value_or("2023-06-01")));
        if (auto beta = ingress_.http.header("anthropic-beta")) request.headers.emplace_back("anthropic-beta", std::string(*beta));
        if (!target_->api_key.empty()) request.headers.emplace_back("x-api-key", target_->api_key);
    } else if (!target_->api_key.empty()) request.headers.emplace_back("authorization", "Bearer " + target_->api_key);
    request.headers.emplace_back("x-request-id", ingress_.request_id);
    request.body = std::move(upstream_body);

    if (meta_.stream) {
        if (upstream_protocol_ == proto::Protocol::openai)
            stream_observer_ = std::make_unique<proto::OpenAiStreamObserver>(meter_.get(), meta_.client_wants_usage, meta_.client_requested_metering);
        decoder_ = std::make_unique<proto::StreamDecoder>(upstream_protocol_);
        encoder_ = std::make_unique<proto::StreamEncoder>(ingress_.protocol, meta_.model, ingress_.request_id, meta_.client_wants_usage);
    }

    last_connect_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(result->connect_time).count();
    last_conn_reused_ = result->reused;

    stage_ = Stage::await_head;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    const int epoch = attempt_epoch_;
    exchange_ = std::make_shared<http::UpstreamExchange>(
        std::move(*result), target_->url, std::move(request),
        http::UpstreamExchange::Handler{
            .on_head = [weak, epoch](int status, const http::Headers& headers) {
                if (auto self = weak.lock(); self && self->attempt_epoch_ == epoch) {
                    self->onUpstreamHead(status, headers);
                }
            },
            .on_body = [weak, epoch](std::string_view data) {
                if (auto self = weak.lock(); self && self->attempt_epoch_ == epoch) self->onUpstreamBody(data);
            },
            .on_complete = [weak, epoch](bool reusable) {
                if (auto self = weak.lock(); self && self->attempt_epoch_ == epoch) {
                    self->onUpstreamComplete(reusable);
                }
            },
            .on_error = [weak, epoch](http::UpstreamIoError error) {
                if (auto self = weak.lock(); self && self->attempt_epoch_ == epoch) {
                    self->onUpstreamError(std::move(error));
                }
            }});
    request_written_ = nowSteady();
    exchange_->start();
    startTtftTimer();
}

void ProxySession::startTtftTimer() {
    auto conn = downstream();
    if (!conn) return;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    ttft_timer_ = conn->getLoop()->runAfter(target_->timeouts.ttft, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        SPDLOG_DEBUG("proxy: ttft timeout request_id={} stage={}", self->ingress_.request_id, toString(self->stage_));
        self->attemptFailure(classifyTransport(TransportFailure::closed_before_head), 0,
                             "upstream ttft timeout", true, std::nullopt);
    });
}

void ProxySession::startIdleTimer() {
    auto conn = downstream();
    if (!conn) return;
    cancelTimer(idle_timer_);
    // 背压暂停期间停表：是我们自己不读，不是上游卡住（proxy-session.md §4.2）；恢复读时重新计时
    if (backpressure_paused_) return;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    idle_timer_ = conn->getLoop()->runAfter(target_->timeouts.idle, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        SPDLOG_DEBUG("proxy: idle timeout request_id={} committed={}", self->ingress_.request_id, self->committed_);
        if (self->stream_completed_) {
            self->finish(Outcome::ok);
            return;
        }
        if (self->committed_) {
            self->failCommitted(504, "upstream idle timeout");
        } else {
            self->attemptFailure(FailureClass{true, true, true}, 0, "upstream idle timeout", true,
                                 std::nullopt);
        }
    });
}

void ProxySession::startStallTimer() {
    auto conn = downstream();
    if (!conn) return;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    stall_timer_ = conn->getLoop()->runAfter(context_.config->stream.stall, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        SPDLOG_WARN("proxy: downstream_stalled request_id={} paused_for={}ms", self->ingress_.request_id,
                    std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady() - self->pause_start_).count());
        self->abortUpstream();
        self->finish(Outcome::downstream_stalled);
        // 先 finish 再关下游：关闭会同步触发下游 close 回调，会话已结束就不会再被记成 cancelled
        if (auto c = self->downstream()) c->forceClose();
    });
}

void ProxySession::startTotalTimer() {
    auto conn = downstream();
    if (!conn) return;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    total_timer_ = conn->getLoop()->runAfter(target_->timeouts.total, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        self->attemptFailure(classifyTransport(TransportFailure::connect_timeout), 0,
                             "upstream total timeout", true, std::nullopt);
    });
}

void ProxySession::startPrecommitTimer() {
    auto conn = downstream();
    if (!conn) return;
    std::chrono::milliseconds limit{100000};
    if (!route_->targets.empty()) {
        if (auto* upstream = context_.config->findUpstream(route_->targets.front())) {
            limit = upstream->timeouts.precommit;
        }
    }
    precommit_deadline_ = nowSteady() + limit;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    precommit_timer_ = conn->getLoop()->runAfter(limit, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_ || self->committed_) return;
        SPDLOG_DEBUG("proxy: precommit timeout request_id={}", self->ingress_.request_id);
        // 提交前总上限到期：立即中止当前尝试，不再开始新尝试（proxy-session.md §3）
        self->attemptFailure(FailureClass{true, false, true}, 0, "precommit timeout", false, std::nullopt, {},
                             /*stop_retry=*/true);
    });
}

void ProxySession::cancelTimer(net::TimerId& id) {
    if (!id.valid()) return;
    if (auto conn = downstream()) conn->getLoop()->cancel(id);
    id = net::TimerId{};
}

void ProxySession::cancelTimers() {
    for (net::TimerId* id : {&ttft_timer_, &idle_timer_, &stall_timer_, &total_timer_, &precommit_timer_}) {
        cancelTimer(*id);
    }
}

void ProxySession::onUpstreamHead(int status, const http::Headers& headers) {
    if (finished_) return;
    upstream_status_ = status;
    upstream_headers_ = headers;
    stage_ = Stage::await_first_event;
    if (status >= 400) return;  // 错误体在 onUpstreamComplete 里统一处理（重试/透传）
    if (!meta_.stream) {
        cancelTimer(ttft_timer_);
        startTotalTimer();
    }
}

void ProxySession::onUpstreamBody(std::string_view data) {
    if (finished_) return;
    if (upstream_status_ >= 400) {
        if (nonstream_body_.size() < 64 * 1024) nonstream_body_.append(data);
        return;
    }
    if (!meta_.stream) {
        if (nonstream_body_.size() + data.size() > 16 * 1024 * 1024) {
            onUpstreamError(http::UpstreamIoError{http::UpstreamIoError::Kind::parse_error, "response too large"});
            return;
        }
        nonstream_body_.append(data);
        return;
    }
    // 收到首个进度事件后 ttft 改按 idle 计时，每次收到字节重置（proxy-session.md §3）
    if (first_progress_seen_ || committed_) startIdleTimer();
    in_sse_feed_ = true;
    sse_parser_.feed(data, [this](http::SseEvent&& event) { handleSseEvent(std::move(event)); });
    in_sse_feed_ = false;
}

void ProxySession::handleSseEvent(http::SseEvent&& event) {
    if (finished_ || !decoder_ || stream_completed_) return;
    try {
        proto::OpenAiStreamObserver::Result observed;
        if (stream_observer_) observed = stream_observer_->process(event.data);
        else proto::observeAnthropicUsage(nlohmann::json::parse(event.data), *meter_);
        auto events = decoder_->feed(event.event, event.data);
        for (const auto& e : events) {
            if (const auto* f = std::get_if<proto::Finish>(&e); f && f->reason == proto::FinishReason::error) {
                if (committed_) failCommitted(502, f->raw);
                else attemptFailure(classifyPrecommitStreamFailure(), 0, f->raw, true, std::nullopt);
                return;
            }
            if (proto::isFirstTokenEvent(e)) meter_->markOutputSeen();
        }
        onQuotaMeterUpdate();
        if (finished_) return;
        if (!first_progress_seen_ && ((stream_observer_ && stream_observer_->sawProgress()) ||
                                     (upstream_protocol_ == proto::Protocol::anthropic && meter_->value().prompt))) {
            first_progress_seen_ = true; cancelTimer(ttft_timer_); startIdleTimer();
        }
        const auto& u = meter_->value();
        encoder_->initialUsage({u.prompt.value_or(0), u.completion.value_or(0), u.cached.value_or(0)});
        std::string framed;
        for (const auto& e : events) {
            if (proto::isFirstTokenEvent(e) && ttft_ == Duration::zero()) ttft_ = nowSteady() - ingress_.received_at;
            auto encoded = encoder_->feed(e);
            if (ingress_.protocol != upstream_protocol_) framed += encoded;
        }
        if (ingress_.protocol == upstream_protocol_) {
            if (stream_observer_) {
                if (observed.action == proto::OpenAiStreamObserver::Result::Action::forward ||
                    observed.action == proto::OpenAiStreamObserver::Result::Action::done) framed = "data: " + observed.data + "\n\n";
            } else framed = (event.event.empty() ? "" : "event: " + event.event + "\n") + "data: " + event.data + "\n\n";
        }
        if (!framed.empty()) {
            if (ingress_.protocol == proto::Protocol::anthropic && upstream_protocol_ == proto::Protocol::anthropic) {
                auto body = nlohmann::json::parse(event.data);
                if (!committed_ && body.value("type", "") == "ping") return;
                anthropic_state_.observe(body);
            }
            if (!committed_) commitStream();
            stageCacheBytes(framed);
            http::ResponseWriter writer(downstream_, ingress_.request_id, true);
            writer.writeChunk(framed);
        }
        if (decoder_->done()) {
            stream_completed_ = true;
            if (!committed_) commitStream();
            http::ResponseWriter writer(downstream_, ingress_.request_id, true);
            writer.endStream(); endstream_sent_ = true;
            if (exchange_ && exchange_->finished()) { finalizeAttempt("ok", BreakerAction::success); finish(Outcome::ok); }
        }
    } catch (const std::exception& e) {
        if (committed_) failCommitted(502, e.what());
        else attemptFailure(classifyPrecommitStreamFailure(), 0, e.what(), true, std::nullopt);
    }
}

void ProxySession::commitStream() {
    if (committed_) return;
    committed_ = true;
    stage_ = Stage::streaming;
    http_status_ = 200;
    cancelTimer(ttft_timer_);
    startIdleTimer();
    if (context_.metrics) context_.metrics->addActiveStreams(1);
    http::Headers headers;
    headers.emplace_back("x-gateway-upstream", target_ ? target_->name : "");
    headers.emplace_back("x-gateway-attempts", std::to_string(attempts_));
    if (cache_miss_) headers.emplace_back("x-gateway-cache", "MISS");
    for (const auto& h : rateHeaders()) headers.push_back(h);
    http::ResponseWriter writer(downstream_, ingress_.request_id, true);
    writer.writeStreamHead(200, headers);
    if (ttft_ == Duration::zero()) ttft_ = nowSteady() - ingress_.received_at;
}

void ProxySession::handleUpstreamErrorBody() {
    auto retry_after = retryAfterFromHeaders(upstream_headers_, nowSteady());
    std::string message = proto::extractErrorMessage(nonstream_body_);
    std::string body;
    if (ingress_.protocol == upstream_protocol_) {
        body = nonstream_body_.empty() ? proto::errorBody(ingress_.protocol, message, "upstream_error")
                                       : nonstream_body_;
    } else {
        body = proto::errorBody(ingress_.protocol, message, "api_error");
    }
    attemptFailure(classifyHttpStatus(upstream_status_), upstream_status_, message, false, retry_after,
                   std::move(body));
}

void ProxySession::handleNonStreamBody() {
    if (upstream_status_ >= 400) {
        handleUpstreamErrorBody();
        return;
    }
    if (meter_) {
        try {
            auto body = nlohmann::json::parse(nonstream_body_);
            auto usage = upstream_protocol_ == proto::Protocol::openai ? proto::extractOpenAiUsage(body) : proto::extractAnthropicUsage(body);
            if (usage.present) {
                meter_->onChunkUsage(usage.prompt, usage.completion, usage.cached);
                meter_->markFinalUsage();
            }
            if (count_tokens_) meter_->markRejected();
            else if (ingress_.protocol != upstream_protocol_)
                nonstream_body_ = proto::translateResponse(body, upstream_protocol_, ingress_.protocol, meta_.model, ingress_.request_id).dump();
        } catch (const std::exception& e) {
            attemptFailure(classifyPrecommitStreamFailure(), 0, std::string("invalid upstream response: ") + e.what(), true, std::nullopt);
            return;
        }
    }
    finalizeAttempt("ok", BreakerAction::success);
    http_status_ = 200;
    http::Headers headers;
    headers.emplace_back("x-gateway-upstream", target_ ? target_->name : "");
    headers.emplace_back("x-gateway-attempts", std::to_string(attempts_));
    if (cache_miss_) headers.emplace_back("x-gateway-cache", "MISS");
    for (const auto& h : rateHeaders()) headers.push_back(h);
    http::ResponseWriter writer(downstream_, ingress_.request_id, true);
    writer.writeFull(200, headers, "application/json", nonstream_body_);
    finish(Outcome::ok);
}

void ProxySession::onUpstreamComplete(bool reusable) {
    if (finished_) return;
    upstream_reusable_ = reusable;
    if (upstream_status_ >= 400) {
        handleNonStreamBody();
        return;
    }
    if (meta_.stream) {
        if (!stream_completed_) {
            if (committed_) {
                failCommitted(502, "upstream stream interrupted");
            } else {
                attemptFailure(classifyPrecommitStreamFailure(), 0, "upstream closed before stream completed",
                               true, std::nullopt);
            }
            return;
        }
        if (!endstream_sent_) {
            http::ResponseWriter writer(downstream_, ingress_.request_id, true);
            writer.endStream();
            endstream_sent_ = true;
        }
        finalizeAttempt("ok", BreakerAction::success);
        finish(Outcome::ok);
        return;
    }
    handleNonStreamBody();
}

void ProxySession::onUpstreamError(http::UpstreamIoError error) {
    if (finished_) return;
    // 复用的空闲连接在收到任何响应字节前断开：对端恰好关闭了空闲连接。换新连接重发一次，
    // 不计尝试次数、不计熔断（net-reactor.md §11）。回调来自 exchange 自身，调用方持有它的强引用，这里可以释放
    if (error.kind == http::UpstreamIoError::Kind::closed_before_head && !reuse_retried_ && exchange_ &&
        exchange_->lease().reused && !exchange_->receivedAnyByte()) {
        reuse_retried_ = true;
        SPDLOG_DEBUG("proxy: reused connection failed before first byte, resending on a new connection request_id={}",
                     ingress_.request_id);
        cancelTimer(ttft_timer_);
        exchange_.reset();
        startAttempt(false);
        return;
    }
    if (!committed_) {
        attemptFailure(classifyTransport(TransportFailure::io_error), 0, error.message, true, std::nullopt);
        return;
    }
    failCommitted(502, error.message);
}

void ProxySession::failCommitted(int status, const std::string& message) {
    if (finished_) return;
    error_ = message;
    abortUpstream();
    // 提交后的中断按失败分类表计入熔断（不重试）
    finalizeAttempt("fatal", BreakerAction::failure);
    if (meta_.stream) {
        nlohmann::json err;
        err["error"]["message"] = "upstream stream interrupted: " + message;
        err["error"]["type"] = "upstream_error";
        if (auto conn = downstream()) {
            http::ResponseWriter writer(downstream_, ingress_.request_id, true);
            if (ingress_.protocol == proto::Protocol::anthropic)
                writer.writeChunk("event: error\ndata: " + proto::anthropicErrorBody(err["error"]["message"].get<std::string>(), "api_error") + "\n\n");
            else writer.writeChunk("data: " + err.dump() + "\n\n");
            writer.endStream();
            endstream_sent_ = true;
        }
    } else {
        http_status_ = status;
    }
    finish(Outcome::stream_interrupted);
}

void ProxySession::abortUpstream(const std::function<void()>& on_shutdown) {
    if (!exchange_ || exchange_->finished()) {
        if (on_shutdown) on_shutdown();
        return;
    }
    if (in_sse_feed_ || !decoder_) {
        // 正处在上游数据的 SSE 解析回调里（流内 error 事件）不能重入解析器；非流式响应体只在完整时才有 usage。
        // 这两种情况直接关闭，不排空
        exchange_->forceClose();
        if (on_shutdown) on_shutdown();
        return;
    }
    // 排空的数据可能接近 1MB（下游或本 loop 落后时上游已发出大量 token），在 loop 线程里逐事件解析 JSON 代价太高。
    // 上游的计数（timings / usage / prompt_progress）都是累计值，所以只扫描 SSE 分帧、记下最后一个 JSON 事件，
    // 排空结束后只把它交给计量，结果与逐事件处理相同
    std::string last_event, last_start, last_delta;
    exchange_->abortAndDrain(
        [this, &last_event, &last_start, &last_delta](std::string_view data) {
            sse_parser_.feed(data, [&last_event, &last_start, &last_delta](http::SseEvent&& event) {
                if (event.event == "message_start") last_start = event.data;
                if (event.event == "message_delta") last_delta = event.data;
                if (event.data != "[DONE]") last_event = std::move(event.data);
            });
        },
        on_shutdown);
    if (!last_event.empty()) {
        if (stream_observer_) stream_observer_->process(last_event);
        else {
            for (const auto* data : {&last_start, &last_delta}) if (!data->empty()) {
                auto j = nlohmann::json::parse(*data, nullptr, false);
                if (!j.is_discarded()) proto::observeAnthropicUsage(j, *meter_);
            }
        }
        const auto& usage = meter_->value();
        SPDLOG_DEBUG("proxy: drained request_id={} computed={} completion={}", ingress_.request_id,
                     usage.prompt_computed ? std::to_string(*usage.prompt_computed) : "null",
                     usage.completion ? std::to_string(*usage.completion) : "null");
    }
}

void ProxySession::finalizeMeter(bool completed) {
    if (meter_ && !meter_finalized_) {
        meter_->finish(completed);
        meter_finalized_ = true;
    }
}

void ProxySession::finalizeAttempt(const char* result, BreakerAction breaker) {
    if (attempt_recorded_ || meter_ == nullptr || target_ == nullptr) return;
    attempt_recorded_ = true;
    bool completed = stream_completed_ || (std::string_view(result) == "ok" && !meta_.stream);
    if (!completed) {
        const auto& usage = meter_->value();
        if (!usage.prompt && !usage.completion && !usage.cached && !usage.prompt_computed &&
            (!exchange_ || upstream_status_ >= 400 ||
             (target_->metering == "llamacpp" && !exchange_->receivedAnyByte()))) {
            meter_->markRejected();  // 建连失败/上游 4xx：没有开始处理，全部为 0
        }
    }
    finalizeMeter(completed);

    AttemptRecord rec;
    rec.upstream = target_->name;
    rec.kind = target_->kind;
    rec.result = result;
    rec.http_status = upstream_status_ != 0 ? upstream_status_ : http_status_;
    rec.usage = meter_->value();
    attempt_list_.push_back(std::move(rec));

    if (context_.metrics) {
        context_.metrics->observeAttempt(target_->name, result);
        if (exchange_ && exchange_->upstreamTtfb() != Duration::zero()) {
            context_.metrics->observeUpstreamTtfb(target_->name, exchange_->upstreamTtfb());
        }
    }
    CircuitBreaker* cb = context_.breakers ? context_.breakers->get(target_->name) : nullptr;
    if (cb != nullptr) {
        if (breaker == BreakerAction::success) {
            cb->recordSuccess();
        } else if (breaker == BreakerAction::failure) {
            cb->recordFailure();
        }
    }
}

void ProxySession::attemptFailure(const FailureClass& cls, int http_status, std::string message, bool close,
                                  std::optional<Duration> retry_after, std::string error_body, bool stop_retry) {
    if (finished_) return;
    last_failure_status_ = http_status;
    last_failure_message_ = message;
    last_failure_body_ = std::move(error_body);
    last_failure_retry_after_ = retry_after;
    last_failure_timeout_ = cls.is_timeout;
    last_failure_close_ = close;
    error_ = message;
    // 结束当前尝试：未读完的排空计量；已读完（如 HTTP 错误体）的直接关闭，不归还连接池
    if (exchange_) {
        if (!exchange_->finished()) {
            abortUpstream();
        } else {
            exchange_->forceClose();
        }
    }
    finalizeAttempt(cls.retryable ? "retryable" : "fatal",
                    cls.counts_toward_breaker ? BreakerAction::failure : BreakerAction::none);

    // 不可重试：直接按最后一次失败返回（跨协议时由 G14 转换）
    if (!cls.retryable) {
        finishAttemptsExhausted();
        return;
    }
    // Retry-After 超过 max_retry_after_ms：不再等，直接降级
    if (retry_after && *retry_after > std::chrono::milliseconds(route_->retry.max_retry_after_ms)) {
        SPDLOG_DEBUG("proxy: retry-after {}ms exceeds max_retry_after_ms, falling back request_id={}",
                     std::chrono::duration_cast<std::chrono::milliseconds>(*retry_after).count(),
                     ingress_.request_id);
        nextTarget();
        return;
    }
    // 提交前总上限到期：不再开始新的尝试
    if (stop_retry || (precommit_deadline_ != Timestamp{} && nowSteady() >= precommit_deadline_)) {
        finishAttemptsExhausted();
        return;
    }
    // 同一目标内还有尝试次数：退避后重试
    if (attempt_in_target_ < route_->retry.max_attempts) {
        Duration delay = backoffDelay(route_->retry, attempt_in_target_);
        if (retry_after && *retry_after > delay) delay = *retry_after;
        ++attempt_in_target_;
        startBackoff(delay);
        return;
    }
    // 该目标尝试耗尽：降级到下一个目标（换目标不等待）
    nextTarget();
}

void ProxySession::nextTarget() {
    ++target_index_;
    attempt_in_target_ = 1;
    if (!selectTarget()) {
        finishAttemptsExhausted();
        return;
    }
    fallback_ = true;
    startAttempt();
}

void ProxySession::finishAttemptsExhausted() {
    if (finished_) return;
    int status = 502;
    std::string body;
    http::Headers extra;
    if (last_failure_status_ >= 400) {
        // 最后一次是上游 HTTP 状态：原样返回（保留 Retry-After）
        status = last_failure_status_;
        body = last_failure_body_.empty()
                   ? proto::errorBody(ingress_.protocol, last_failure_message_, "upstream_error")
                   : last_failure_body_;
        if (last_failure_retry_after_) {
            auto seconds = std::chrono::ceil<std::chrono::seconds>(*last_failure_retry_after_);
            extra.emplace_back("retry-after", std::to_string(seconds.count()));
        }
    } else if (last_failure_timeout_) {
        status = 504;
        body = proto::errorBody(ingress_.protocol, last_failure_message_, "upstream_timeout");
    } else {
        status = 502;
        body = proto::errorBody(ingress_.protocol, last_failure_message_, "upstream_error");
    }
    writeErrorResponse(status, body, last_failure_status_ < 400, extra);
    finish(Outcome::upstream_error);
}

void ProxySession::startBackoff(Duration delay) {
    stage_ = Stage::backoff;
    cancelTimer(ttft_timer_);
    cancelTimer(idle_timer_);
    cancelTimer(total_timer_);
    cancelTimer(stall_timer_);
    auto conn = downstream();
    if (!conn) return;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delay);
    if (ms < std::chrono::milliseconds(1)) ms = std::chrono::milliseconds(1);
    SPDLOG_DEBUG("proxy: backoff request_id={} delay_ms={} target={} next_attempt={}", ingress_.request_id,
                 ms.count(), target_ ? target_->name : "-", attempt_in_target_);
    std::weak_ptr<ProxySession> weak = shared_from_this();
    backoff_timer_ = conn->getLoop()->runAfter(ms, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        self->startAttempt();
    });
}

std::optional<std::int64_t> ProxySession::totalConsumed() const {
    std::int64_t total = 0;
    for (const auto& attempt : attempt_list_) {
        auto consumed = attempt.usage.consumed();
        if (!consumed) return std::nullopt;
        total += *consumed;
    }
    return total;
}

std::pair<std::int64_t, std::int64_t> ProxySession::quotaTotals() const {
    std::int64_t consumed = 0;
    std::int64_t projected = 0;
    for (const auto& attempt : attempt_list_) {
        if (auto c = attempt.usage.consumed()) consumed += *c;
        if (auto p = attempt.usage.projected()) projected += *p;
    }
    if (meter_ && !attempt_recorded_) {
        if (auto c = meter_->value().consumed()) consumed += *c;
        if (auto p = meter_->value().projected()) projected += *p;
    }
    return {consumed, projected};
}

void ProxySession::writeErrorResponse(int status, const std::string& body, bool close,
                                      const http::Headers& extra_headers) {
    auto conn = downstream();
    if (!conn || !conn->connected()) return;
    http_status_ = status;
    http::Headers headers;
    if (target_) headers.emplace_back("x-gateway-upstream", target_->name);
    headers.emplace_back("x-gateway-attempts", std::to_string(attempts_));
    for (const auto& h : extra_headers) headers.push_back(h);
    http::ResponseWriter writer(downstream_, ingress_.request_id, !close);
    writer.writeFull(status, headers, "application/json", body);
    if (close) conn->shutdown();
}

void ProxySession::cancel(std::string reason) {
    if (finished_) return;
    cancelled_ = true;
    cancel_reason_ = std::move(reason);
    error_ = cancel_reason_;
    auto start = nowSteady();
    // 取消延迟只统计到向上游发出 FIN 为止（排空已到达的数据不计入）
    auto cancel_latency = std::chrono::microseconds{0};
    auto mark_latency = [&] {
        cancel_latency = std::chrono::duration_cast<std::chrono::microseconds>(nowSteady() - start);
    };
    switch (stage_) {
        case Stage::acquiring:
            if (acquire_handle_) acquire_handle_->cancel();
            mark_latency();
            break;
        case Stage::await_head:
        case Stage::await_first_event:
        case Stage::streaming:
            abortUpstream(mark_latency);
            break;
        case Stage::backoff:
            cancelTimer(backoff_timer_);
            mark_latency();
            break;
        case Stage::admitting:
        case Stage::done:
            mark_latency();
            break;
    }
    cancel_latency_us_ = static_cast<std::int64_t>(cancel_latency.count());
    // 取消不算成功/失败：只收尾计量并释放半开探测名额
    finalizeAttempt("fatal", BreakerAction::none);
    if (target_ != nullptr && context_.breakers != nullptr) {
        if (auto* cb = context_.breakers->get(target_->name); cb != nullptr) cb->releaseProbe();
    }
    SPDLOG_DEBUG("proxy: cancelled request_id={} stage={} reason={} cancel_latency_us={}", ingress_.request_id,
                 toString(stage_), cancel_reason_, cancel_latency.count());
    if (context_.metrics && !meta_.model.empty()) {
        context_.metrics->observeCancellation(meta_.model, cancellationStage());
    }
    finish(Outcome::cancelled);
}

void ProxySession::onDownstreamHighWater(std::size_t mark) {
    if (finished_ || !exchange_ || !committed_ || backpressure_paused_) return;
    backpressure_paused_ = true;
    backpressure_pauses_ += 1;
    pause_start_ = nowSteady();
    exchange_->pauseReading();
    cancelTimer(idle_timer_);
    startStallTimer();
    if (context_.metrics) {
        context_.metrics->observeBackpressurePause();
        context_.metrics->addBackpressureActive(1);
    }
    SPDLOG_DEBUG("proxy: backpressure pause request_id={} output={} (pause #{})", ingress_.request_id, mark,
                 backpressure_pauses_);
}

void ProxySession::onDownstreamWriteComplete() {
    if (finished_ || !exchange_ || !backpressure_paused_) return;
    backpressure_paused_ = false;
    backpressure_total_ += nowSteady() - pause_start_;
    cancelTimer(stall_timer_);
    exchange_->resumeReading();
    startIdleTimer();
    if (context_.metrics) context_.metrics->addBackpressureActive(-1);
    SPDLOG_DEBUG("proxy: backpressure resume request_id={}", ingress_.request_id);
}

void ProxySession::finish(Outcome outcome) {
    if (finished_) return;
    finished_ = true;
    stage_ = Stage::done;
    outcome_ = outcome;
    cancelTimers();
    cancelTimer(backoff_timer_);

    auto conn = downstream();
    if (exchange_) {
        bool can_release = upstream_reusable_ && !cancelled_;
        if (can_release && conn) {
            auto* services_ptr = std::any_cast<std::shared_ptr<gateway::IoLoopServices>>(&conn->getLoop()->context());
            if (services_ptr != nullptr && (*services_ptr)->pool != nullptr) {
                (*services_ptr)->pool->release(exchange_->lease());
            } else {
                exchange_->forceClose();
            }
        } else {
            exchange_->forceClose();
        }
    }

    finalizeMeter(stream_completed_);
    if (context_.metrics) {
        if (backpressure_paused_) context_.metrics->addBackpressureActive(-1);
        if (committed_) context_.metrics->addActiveStreams(-1);
    }
    writeCacheEntry();
    // 限流结算：按各尝试 consumed 之和（上游报告的真实总量）多退少补；
    // 任一次尝试 consumed 未知则整体不退还预留，保守处理（policies.md §4.6）
    if (rate_ticket_ && context_.rate_limiter) {
        context_.rate_limiter->settle(*rate_ticket_, totalConsumed());
    }
    // 配额：把尚未上报的增量补上（发出即可，不等回复；取消、失败的尝试也一样）
    if (quota_view_ && redisClient() != nullptr && context_.quota_manager != nullptr) {
        auto [consumed, projected] = quotaTotals();
        (void)projected;
        if (consumed > quota_view_->consumed) quota_view_->consumed = consumed;
        if (quota_view_->consumed > quota_view_->reported) {
            sendQuotaAdd(quota_view_->consumed - quota_view_->reported);
        }
    }
    observeFinish();
    logRecord();

    if (conn) {
        if (auto* state = http::HttpServer::stateOf(conn)) {
            state->session.reset();
        }
        server_.finishRequest(conn);
    }
}

const char* ProxySession::cancellationStage() const {
    switch (stage_) {
        case Stage::acquiring: return "acquiring";
        case Stage::await_head:
        case Stage::await_first_event:
        case Stage::admitting:
        case Stage::done: return "awaiting";
        case Stage::streaming: return "streaming";
        case Stage::backoff: return "backoff";
    }
    return "awaiting";
}

store::RedisClient* ProxySession::redisClient() const {
    auto conn = downstream_.lock();
    if (!conn) return nullptr;
    auto* services = std::any_cast<std::shared_ptr<IoLoopServices>>(&conn->getLoop()->context());
    return services && *services ? (*services)->redis.get() : nullptr;
}

void ProxySession::onQuotaMeterUpdate() {
    if (finished_ || !quota_view_ || quota_view_->truncated) return;
    {
        auto [consumed, projected] = quotaTotals();
        if (consumed > quota_view_->consumed) quota_view_->consumed = consumed;
        if (projected > quota_view_->projected) quota_view_->projected = projected;
    }
    // 本地判断（projected，不等 Redis）：prompt 总数在预填充开始就已知，超大 prompt 在算力花出去前拒绝
    if (quota_view_->overBudget()) {
        if (!committed_) { rejectForQuota(); return; }
        truncateForQuota();
        return;
    }
    // 上报（consumed）：攒批，report_every_tokens 或超时
    int64_t now = wallMillis();
    if (quota_view_->consumed > quota_view_->reported &&
        quota_view_->shouldReport(now, context_.config->quota.report_every_tokens,
                                  static_cast<int>(context_.config->quota.report_interval.count()))) {
        int64_t delta = quota_view_->consumed - quota_view_->reported;
        quota_view_->reported = quota_view_->consumed;
        quota_view_->last_report_ms = now;
        sendQuotaAdd(delta);
    }
}

void ProxySession::rejectForQuota() {
    if (finished_) return;
    abortUpstream();
    finalizeAttempt("fatal", BreakerAction::none);
    http::Headers headers{{"retry-after", std::to_string(std::max<std::int64_t>(1,
        secondsToNextLocalMidnight(wallMillis(), context_.config->quota.utc_offset_minutes)))},
        {"x-gateway-quota-remaining-tokens", "0"}};
    writeErrorResponse(429, proto::errorBody(ingress_.protocol, "token quota exceeded",
        ingress_.protocol == proto::Protocol::anthropic ? "rate_limit_error" : "insufficient_quota",
        "insufficient_quota"), false, headers);
    if (context_.metrics) context_.metrics->observeQuotaRejection(ingress_.user->name);
    finish(Outcome::quota_exceeded);
}

void ProxySession::sendQuotaAdd(std::int64_t delta) {
    auto* redis = redisClient();
    if (redis == nullptr || context_.quota_manager == nullptr || ingress_.user == nullptr || delta <= 0) return;
    const auto& qc = context_.config->quota;
    int64_t day = quotaDayOf(wallMillis(), qc.utc_offset_minutes);
    int64_t month = quotaMonthOf(wallMillis(), qc.utc_offset_minutes);
    std::weak_ptr<ProxySession> weak = shared_from_this();
    const auto reported_at_send = quota_view_ ? quota_view_->reported : 0;
    context_.quota_manager->add(*redis, ingress_.user->name, day, month, delta,
                                [weak, reported_at_send](std::expected<QuotaUsage, store::RedisError> result) {
                                    auto self = weak.lock();
                                    if (!self) return;  // finish 后的 fire-and-forget 回复：忽略
                                    if (!result) {
                                        SPDLOG_WARN("quota_add failed request_id={}: {}", self->ingress_.request_id,
                                                    result.error().message);
                                        return;
                                    }
                                    if (!self->quota_view_ || self->finished_) return;
                                    // 回复里的 used 更新 remaining；超过限额则同超额处理
                                    const auto& q = *self->ingress_.user->quota;
                                    std::optional<std::int64_t> remaining;
                                    if (q.daily_tokens > 0) remaining = q.daily_tokens - result->used_day;
                                    if (q.monthly_tokens > 0) {
                                        int64_t mr = q.monthly_tokens - result->used_month;
                                        remaining = remaining ? std::min(*remaining, mr) : mr;
                                    }
                                    if (remaining) *remaining -= self->quota_view_->reported - reported_at_send;
                                    self->quota_view_->remaining = remaining;
                                    if (remaining.has_value() && *remaining <= 0) {
                                        if (self->committed_) {
                                            self->truncateForQuota();
                                        } else {
                                            self->rejectForQuota();
                                        }
                                    }
                                });
}

void ProxySession::truncateForQuota() {
    if (finished_ || quota_truncated_) return;
    quota_truncated_ = true;
    if (quota_view_) quota_view_->truncated = true;
    error_ = "quota exceeded, response truncated";
    // 按取消的路径停止上游（abortAndDrain 计量），按协议正常收尾下游（usage-ledger.md §6）
    abortUpstream();
    finalizeAttempt("fatal", BreakerAction::none);
    if (meta_.stream && committed_) {
        http::ResponseWriter writer(downstream_, ingress_.request_id, true);
        if (ingress_.protocol == proto::Protocol::anthropic && encoder_) {
            const auto& u = meter_->value();
            proto::Usage usage{u.prompt.value_or(0), u.completion.value_or(0), u.cached.value_or(0)};
            if (upstream_protocol_ == proto::Protocol::anthropic) writer.writeChunk(anthropic_state_.finish(usage));
            else {
                encoder_->feed(usage);
                writer.writeChunk(encoder_->feed(proto::Finish{proto::FinishReason::length, "max_tokens"}));
            }
        } else {
        nlohmann::json chunk;
        nlohmann::json choice;
        choice["delta"] = nlohmann::json::object();
        choice["finish_reason"] = "length";
        choice["index"] = 0;
        chunk["choices"] = nlohmann::json::array({choice});
        writer.writeChunk("data: " + chunk.dump() + "\n\n");
        if (meta_.client_wants_usage && meter_) {
            const auto& u = meter_->value();
            if (u.prompt && u.completion) {
                nlohmann::json usage;
                usage["prompt_tokens"] = *u.prompt;
                usage["completion_tokens"] = *u.completion;
                usage["total_tokens"] = *u.prompt + *u.completion;
                nlohmann::json usage_chunk;
                usage_chunk["choices"] = nlohmann::json::array();
                usage_chunk["usage"] = usage;
                writer.writeChunk("data: " + usage_chunk.dump() + "\n\n");
            }
        }
        writer.writeChunk("data: [DONE]\n\n");
        }
        writer.endStream();
        endstream_sent_ = true;
    }
    if (context_.metrics) context_.metrics->observeQuotaTruncation(ingress_.user->name);
    finish(Outcome::quota_truncated);
}

void ProxySession::observeFinish() {
    if (!context_.metrics && !context_.access_log && !context_.recent) return;

    RequestRecord record;
    record.ts = wall_ts_;
    record.request_id = ingress_.request_id;
    record.user = ingress_.user ? ingress_.user->name : "-";
    record.ingress = ingress_.protocol == proto::Protocol::anthropic ? "anthropic" : "openai";
    record.model = meta_.model.empty() ? "-" : meta_.model;
    record.stream = meta_.stream;
    record.outcome = toString(outcome_);
    record.http_status = http_status_;
    auto ms = [](Duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    record.latency_ms = ms(nowSteady() - ingress_.received_at);
    if (meta_.stream && ttft_ != Duration::zero()) record.ttft_ms = ms(ttft_);
    if (exchange_) record.upstream_ttfb_ms = ms(exchange_->upstreamTtfb());
    record.connect_ms = last_connect_ms_;
    record.conn_reused = last_conn_reused_;
    record.cache = cache_hit_ ? "hit" : (cache_miss_ ? "miss" : "none");
    record.backpressure_pauses = backpressure_pauses_;
    record.backpressure_ms = ms(backpressure_total_);
    if (cancelled_) record.cancel_latency_us = cancel_latency_us_;

    record.fallback = fallback_;
    record.attempt_list = attempt_list_;
    record.attempts = static_cast<int>(attempt_list_.size());
    record.upstream = target_ ? target_->name : "";
    record.upstream_kind = target_ ? target_->kind : "";

    // 请求级汇总：各尝试之和，任一字段未报告即 null（policies.md §4.6）
    auto sumField = [&record](std::optional<std::int64_t> AttemptUsage::*field) -> std::optional<std::int64_t> {
        std::int64_t total = 0;
        for (const auto& a : record.attempt_list) {
            const auto& v = a.usage.*field;
            if (!v) return std::nullopt;
            total += *v;
        }
        return total;
    };
    if (!record.attempt_list.empty()) {
        record.prompt_tokens = sumField(&AttemptUsage::prompt);
        record.cached_tokens = sumField(&AttemptUsage::cached);
        record.prompt_computed_tokens = sumField(&AttemptUsage::prompt_computed);
        record.completion_tokens = sumField(&AttemptUsage::completion);
    }

    // usage_source 聚合
    {
        bool any_unreported = false, any_partial = false, any_final = false;
        for (const auto& a : record.attempt_list) {
            switch (a.usage.source) {
                case AttemptUsage::Source::unreported: any_unreported = true; break;
                case AttemptUsage::Source::upstream_partial: any_partial = true; break;
                case AttemptUsage::Source::upstream_final: any_final = true; break;
                default: break;
            }
        }
        if (record.attempt_list.empty()) record.usage_source = "rejected";
        else if (any_unreported) record.usage_source = "unreported";
        else if (any_partial) record.usage_source = "upstream_partial";
        else if (any_final) record.usage_source = "upstream_final";
        else record.usage_source = "rejected";
        record.usage_complete = true;
        for (const auto& a : record.attempt_list) {
            if (a.usage.source == AttemptUsage::Source::unreported ||
                a.usage.source == AttemptUsage::Source::none) {
                record.usage_complete = false;
            }
        }
    }

    // 成本：各尝试按实际服务它的上游单价计算后求和；任一尝试成本未知则整体为 null
    {
        std::int64_t total_micro = 0;
        bool known = true;
        for (const auto& a : record.attempt_list) {
            auto cost = attemptCostMicroUsd(context_.config->findPricing(a.upstream), a.usage);
            if (!cost) {
                known = false;
                break;
            }
            total_micro += *cost;
        }
        if (known && !record.attempt_list.empty()) record.cost_usd = microUsdToUsd(total_micro);
    }

    record.error = error_;

    if (cache_hit_ && cache_entry_) {
        // 缓存命中：usage 取条目保存的原始上游值，source=cache，cost=0，saved_cost=按条目 usage 计算
        record.upstream = cache_entry_->upstream;
        record.upstream_kind = "";
        record.usage_source = "cache";
        record.prompt_tokens = cache_entry_->usage.prompt;
        record.cached_tokens = cache_entry_->usage.cached;
        record.prompt_computed_tokens = cache_entry_->usage.prompt_computed;
        record.completion_tokens = cache_entry_->usage.completion;
        record.usage_complete = true;
        record.cost_usd = 0.0;
        if (auto saved = attemptCostMicroUsd(context_.config->findPricing(cache_entry_->upstream),
                                             cache_entry_->usage)) {
            record.saved_cost_usd = microUsdToUsd(*saved);
            if (context_.metrics) context_.metrics->observeCacheSavedCost(meta_.model, *saved);
        }
    }

    if (record.latency_ms >= context_.config->log.slow.count()) {
        SPDLOG_WARN("slow request request_id={} model={} outcome={} latency_ms={}", record.request_id, record.model,
                    record.outcome, record.latency_ms);
    }

    // 账本入账：鉴权通过后的每个请求都留痕（被拒绝/取消/缓存命中的行 billable 可能为 0）
    if (context_.ledger_writer != nullptr) {
        UsageRow row;
        row.ledger_id = gw::randomHex(16);
        row.client_request_id = record.request_id;
        row.gateway_instance = context_.config->instance_id;
        row.key_name = record.user;
        row.billing_day = quotaBillingDay(record.ts, context_.config->quota.utc_offset_minutes);
        row.started_at_ms = record.ts;
        row.finished_at_ms = wallMillis();
        row.ingress = record.ingress;
        row.model = record.model;
        row.upstream = record.upstream;
        row.stream = record.stream;
        row.outcome = record.outcome;
        row.http_status = record.http_status;
        row.input_tokens = record.prompt_tokens;
        row.cached_tokens = record.cached_tokens;
        row.computed_input_tokens = record.prompt_computed_tokens;
        row.output_tokens = record.completion_tokens;
        std::int64_t billable = 0;
        for (const auto& a : record.attempt_list) {
            billable += a.usage.consumed().value_or(0);  // consumed 为 null 的尝试按 0 计
        }
        row.billable_tokens = billable;
        row.usage_complete = record.usage_complete;
        row.cost_micro_usd = record.cost_usd ? std::optional<std::int64_t>(
                                                   static_cast<std::int64_t>(*record.cost_usd * 1e6 + 0.5))
                                             : std::nullopt;
        row.truncated = quota_truncated_;
        row.latency_ms = record.latency_ms;
        row.ttft_ms = record.ttft_ms;
        for (const auto& a : record.attempt_list) row.attempts.push_back(a.toJson());
        context_.ledger_writer->submit(std::move(row));
    }

    if (context_.metrics) context_.metrics->observeRequest(record);
    if (context_.access_log) context_.access_log->write(record);
    if (context_.recent) context_.recent->push(std::make_shared<const RequestRecord>(std::move(record)));
}

void ProxySession::logRecord() {
    const AttemptUsage usage = meter_ ? meter_->value() : AttemptUsage{};
    auto ms = [](Duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    SPDLOG_DEBUG(
        "proxy record request_id={} model={} upstream={} outcome={} status={} stream={} attempts={} "
        "prompt={} cached={} computed={} completion={} usage_source={} ttft_ms={} latency_ms={} "
        "upstream_ttfb_ms={} backpressure_pauses={} cancel_reason={}",
        ingress_.request_id, meta_.model, target_ ? target_->name : "-", toString(outcome_), http_status_,
        meta_.stream, attempts_,
        usage.prompt ? std::to_string(*usage.prompt) : "null",
        usage.cached ? std::to_string(*usage.cached) : "null",
        usage.prompt_computed ? std::to_string(*usage.prompt_computed) : "null",
        usage.completion ? std::to_string(*usage.completion) : "null", static_cast<int>(usage.source), ms(ttft_),
        ms(nowSteady() - ingress_.received_at),
        ms(exchange_ ? exchange_->upstreamTtfb() : Duration::zero()), backpressure_pauses_, cancel_reason_);
}

}  // namespace gw::gateway
