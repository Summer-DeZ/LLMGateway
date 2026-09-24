#include "gateway/proxy_session.hpp"

#include <algorithm>

#include "base/log.hpp"
#include "gateway/usage_meter.hpp"
#include "http/response_writer.hpp"
#include "proto/errors.hpp"

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
    // 3a 账本健康检查（G11）、3b 配额（G11）、4 限流（G10）、5 缓存（G13）、6 尝试计划（G12）
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
    auto meta = proto::parseOpenAiRequest(request_json_);
    if (!meta) {
        writeErrorResponse(400, proto::errorBody(ingress_.protocol, meta.error(), "invalid_request_error"), true);
        finish(Outcome::bad_request);
        return;
    }
    meta_ = *meta;

    route_ = context_.config->findModel(meta_.model);
    if (route_ == nullptr) {
        writeErrorResponse(404,
                           proto::errorBody(ingress_.protocol, "model not found: " + meta_.model,
                                            "invalid_request_error", "model_not_found"),
                           true);
        finish(Outcome::bad_request);
        return;
    }
    startPrecommitTimer();
    startAttempt();
}

void ProxySession::startAttempt() {
    const std::string& target_name = route_->targets.front();
    target_ = context_.config->findUpstream(target_name);
    if (target_ == nullptr) {
        writeErrorResponse(503,
                           proto::errorBody(ingress_.protocol, "upstream not configured", "upstream_unavailable"),
                           true);
        finish(Outcome::unavailable);
        return;
    }
    key_ = http::UpstreamKey{target_->url.tls, target_->url.host, target_->url.port};
    if (!meter_) meter_ = std::make_unique<UsageMeter>(target_->metering);

    auto conn = downstream();
    if (!conn) return;
    net::EventLoop* loop = conn->getLoop();
    auto* pool_ptr = std::any_cast<std::shared_ptr<http::UpstreamPool>>(&loop->context());
    if (pool_ptr == nullptr) {
        writeErrorResponse(503,
                           proto::errorBody(ingress_.protocol, "upstream pool not initialized", "upstream_error"),
                           true);
        finish(Outcome::unavailable);
        return;
    }
    auto pool = *pool_ptr;

    stage_ = Stage::acquiring;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    acquire_handle_ = pool->acquire(key_, target_->timeouts.connect,
                                    [weak](std::expected<http::Lease, http::AcquireError> result) {
                                        if (auto self = weak.lock()) self->onAcquire(std::move(result));
                                    });
}

void ProxySession::onAcquire(std::expected<http::Lease, http::AcquireError> result) {
    if (finished_) return;
    if (!result) {
        if (cancelled_ || result.error().kind == http::AcquireError::Kind::cancelled) {
            finish(Outcome::cancelled);
            return;
        }
        int status = 502;
        std::string type = "upstream_error";
        if (result.error().kind == http::AcquireError::Kind::connect_timeout) {
            status = 504;
            type = "upstream_timeout";
        }
        writeErrorResponse(status,
                           proto::errorBody(ingress_.protocol, "upstream connect failed: " + result.error().message,
                                            type),
                           true);
        finish(Outcome::upstream_error);
        return;
    }

    lease_ = std::move(*result);
    upstream_body_ = proto::rewriteOpenAiRequest(request_json_, target_->model, target_->metering,
                                                 target_->extra_body, meta_.stream)
                         .dump();

    http::UpstreamRequest request;
    request.method = "POST";
    request.path = (target_->url.path_prefix.empty() ? std::string() : target_->url.path_prefix) + "/chat/completions";
    request.headers.emplace_back("content-type", "application/json");
    request.headers.emplace_back("accept-encoding", "identity");
    if (meta_.stream) request.headers.emplace_back("accept", "text/event-stream");
    if (!target_->api_key.empty()) request.headers.emplace_back("authorization", "Bearer " + target_->api_key);
    request.headers.emplace_back("x-request-id", ingress_.request_id);
    request.body = upstream_body_;

    if (meta_.stream) {
        stream_observer_ = std::make_unique<proto::OpenAiStreamObserver>(meter_.get(), meta_.client_wants_usage,
                                                                         meta_.client_requested_metering);
    }

    stage_ = Stage::await_head;
    std::weak_ptr<ProxySession> weak = shared_from_this();
    exchange_ = std::make_shared<http::UpstreamExchange>(
        std::move(lease_), target_->url, std::move(request),
        http::UpstreamExchange::Handler{
            .on_head = [weak](int status, const http::Headers& headers) {
                if (auto self = weak.lock()) self->onUpstreamHead(status, headers);
            },
            .on_body = [weak](std::string_view data) {
                if (auto self = weak.lock()) self->onUpstreamBody(data);
            },
            .on_complete = [weak](bool reusable) {
                if (auto self = weak.lock()) self->onUpstreamComplete(reusable);
            },
            .on_error = [weak](http::UpstreamIoError error) {
                if (auto self = weak.lock()) self->onUpstreamError(std::move(error));
            }});
    request_written_ = nowSteady();
    exchange_->start();
    startTtftTimer();
}

void ProxySession::startTtftTimer() {
    auto conn = downstream();
    if (!conn) return;
    net::EventLoop* loop = conn->getLoop();
    auto limit = timeoutMs(target_ ? target_->timeouts.ttft : std::chrono::milliseconds(90000), 1);
    std::weak_ptr<ProxySession> weak = shared_from_this();
    ttft_timer_ = loop->runAfter(limit, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        SPDLOG_DEBUG("proxy: ttft timeout request_id={} stage={}", self->ingress_.request_id, toString(self->stage_));
        self->failAttempt(504, "upstream ttft timeout", true);
    });
}

void ProxySession::startIdleTimer() {
    auto conn = downstream();
    if (!conn) return;
    net::EventLoop* loop = conn->getLoop();
    auto limit = timeoutMs(target_ ? target_->timeouts.idle : std::chrono::milliseconds(60000), 2);
    if (idle_timer_.valid()) loop->cancel(idle_timer_);
    std::weak_ptr<ProxySession> weak = shared_from_this();
    idle_timer_ = loop->runAfter(limit, [weak] {
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
            self->failAttempt(504, "upstream idle timeout", true);
        }
    });
}

void ProxySession::startStallTimer() {
    auto conn = downstream();
    if (!conn) return;
    net::EventLoop* loop = conn->getLoop();
    std::weak_ptr<ProxySession> weak = shared_from_this();
    stall_timer_ = loop->runAfter(context_.config->stream.stall, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        SPDLOG_WARN("proxy: downstream_stalled request_id={} paused_for={}ms", self->ingress_.request_id,
                    std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady() - self->pause_start_).count());
        if (self->exchange_) {
            self->drainUpstream([self](std::string_view data) { self->feedMeterOnly(data); });
        }
        if (auto c = self->downstream()) c->forceClose();
        self->finish(Outcome::downstream_stalled);
    });
}

void ProxySession::startTotalTimer() {
    auto conn = downstream();
    if (!conn) return;
    net::EventLoop* loop = conn->getLoop();
    auto limit = timeoutMs(target_ ? target_->timeouts.total : std::chrono::milliseconds(600000), 3);
    std::weak_ptr<ProxySession> weak = shared_from_this();
    total_timer_ = loop->runAfter(limit, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        self->failAttempt(504, "upstream total timeout", true);
    });
}

void ProxySession::startPrecommitTimer() {
    auto conn = downstream();
    if (!conn) return;
    net::EventLoop* loop = conn->getLoop();
    std::chrono::milliseconds limit{100000};
    if (route_ && !route_->targets.empty()) {
        if (auto* upstream = context_.config->findUpstream(route_->targets.front())) {
            limit = upstream->timeouts.precommit;
        }
    }
    std::weak_ptr<ProxySession> weak = shared_from_this();
    precommit_timer_ = loop->runAfter(limit, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_ || self->committed_) return;
        SPDLOG_DEBUG("proxy: precommit timeout request_id={}", self->ingress_.request_id);
        self->failAttempt(504, "precommit timeout", false);
    });
}

std::chrono::milliseconds ProxySession::timeoutMs(std::chrono::milliseconds fallback, int which) const {
    if (target_ == nullptr) return fallback;
    switch (which) {
        case 1: return target_->timeouts.ttft;
        case 2: return target_->timeouts.idle;
        case 3: return target_->timeouts.total;
        default: return fallback;
    }
}

void ProxySession::cancelTimers() {
    auto conn = downstream();
    net::EventLoop* loop = conn ? conn->getLoop() : nullptr;
    if (loop == nullptr) return;
    for (net::TimerId* id : {&ttft_timer_, &idle_timer_, &stall_timer_, &total_timer_, &precommit_timer_}) {
        if (id->valid()) {
            loop->cancel(*id);
            *id = net::TimerId{};
        }
    }
}

void ProxySession::onUpstreamHead(int status, const http::Headers& headers) {
    if (finished_) return;
    upstream_status_ = status;
    upstream_headers_ = headers;
    stage_ = Stage::await_first_event;
    if (status >= 400) {
        if (meter_) meter_->markRejected();
        return;
    }
    if (!meta_.stream) {
        if (ttft_timer_.valid()) {
            if (auto conn = downstream()) conn->getLoop()->cancel(ttft_timer_);
            ttft_timer_ = net::TimerId{};
        }
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
    if (idle_timer_.valid() || committed_) startIdleTimer();
    sse_parser_.feed(data, [this](http::SseEvent&& event) { handleSseEvent(std::move(event)); });
}

void ProxySession::handleSseEvent(http::SseEvent&& event) {
    if (finished_ || !stream_observer_) return;
    auto result = stream_observer_->process(event.data);
    switch (result.action) {
        case proto::OpenAiStreamObserver::Result::Action::drop:
            if (!committed_ && stream_observer_->sawProgress() && !first_progress_seen_) {
                first_progress_seen_ = true;
                if (ttft_timer_.valid()) {
                    if (auto conn = downstream()) conn->getLoop()->cancel(ttft_timer_);
                    ttft_timer_ = net::TimerId{};
                }
                startIdleTimer();
            }
            return;
        case proto::OpenAiStreamObserver::Result::Action::error:
            if (committed_) {
                failCommitted(502, "upstream stream error: " + result.error_message);
            } else {
                failAttempt(502, "upstream stream error: " + result.error_message, true);
            }
            return;
        case proto::OpenAiStreamObserver::Result::Action::done:
            stream_completed_ = true;
            if (!committed_) commitStream();
            if (auto conn = downstream()) {
                http::ResponseWriter writer(downstream_, ingress_.request_id, true);
                writer.writeChunk("data: " + result.data + "\n\n");
                writer.endStream();
                endstream_sent_ = true;
            }
            if (exchange_ && exchange_->finished()) finish(Outcome::ok);
            return;
        case proto::OpenAiStreamObserver::Result::Action::forward:
            break;
    }

    if (!committed_) commitStream();
    if (result.first_token && ttft_ == Duration::zero()) {
        ttft_ = nowSteady() - ingress_.received_at;
    }
    if (committed_) {
        http::ResponseWriter writer(downstream_, ingress_.request_id, true);
        writer.writeChunk("data: " + result.data + "\n\n");
    }
}

void ProxySession::commitStream() {
    if (committed_) return;
    committed_ = true;
    stage_ = Stage::streaming;
    http_status_ = 200;
    if (ttft_timer_.valid()) {
        if (auto conn = downstream()) conn->getLoop()->cancel(ttft_timer_);
        ttft_timer_ = net::TimerId{};
    }
    startIdleTimer();
    http::Headers headers;
    headers.emplace_back("x-gateway-upstream", target_ ? target_->name : "");
    headers.emplace_back("x-gateway-attempts", std::to_string(attempts_));
    http::ResponseWriter writer(downstream_, ingress_.request_id, true);
    writer.writeStreamHead(200, headers);
    if (ttft_ == Duration::zero()) ttft_ = nowSteady() - ingress_.received_at;
}

void ProxySession::handleNonStreamBody() {
    if (upstream_status_ >= 400) {
        std::string message = proto::extractErrorMessage(nonstream_body_);
        std::string body;
        if (ingress_.protocol == proto::Protocol::openai) {
            body = nonstream_body_.empty()
                       ? proto::errorBody(ingress_.protocol, message, "upstream_error")
                       : nonstream_body_;
        } else {
            body = proto::errorBody(ingress_.protocol, message, "api_error");
        }
        writeErrorResponse(upstream_status_, body, false);
        finish(Outcome::upstream_error);
        return;
    }
    if (meter_) {
        try {
            auto body = nlohmann::json::parse(nonstream_body_);
            auto usage = proto::extractOpenAiUsage(body);
            if (usage.present) {
                meter_->onChunkUsage(usage.prompt, usage.completion, usage.cached);
                meter_->markFinalUsage();
            }
        } catch (...) {
        }
        finalizeMeter(true);
    }
    http_status_ = 200;
    http::Headers headers;
    headers.emplace_back("x-gateway-upstream", target_ ? target_->name : "");
    headers.emplace_back("x-gateway-attempts", std::to_string(attempts_));
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
                failAttempt(502, "upstream closed before stream completed", true);
            }
            return;
        }
        if (!endstream_sent_) {
            http::ResponseWriter writer(downstream_, ingress_.request_id, true);
            writer.endStream();
            endstream_sent_ = true;
        }
        finalizeMeter(true);
        finish(Outcome::ok);
        return;
    }
    handleNonStreamBody();
}

void ProxySession::onUpstreamError(http::UpstreamIoError error) {
    if (finished_) return;
    if (!committed_) {
        bool timeout = error.kind == http::UpstreamIoError::Kind::parse_error && error.message.find("timeout") != std::string::npos;
        failAttempt(timeout ? 504 : 502, error.message, true);
        return;
    }
    failCommitted(502, error.message);
}

void ProxySession::failCommitted(int status, const std::string& message) {
    if (finished_) return;
    finalizeMeter(false);
    if (meta_.stream) {
        nlohmann::json err;
        err["error"]["message"] = "upstream stream interrupted: " + message;
        err["error"]["type"] = "upstream_error";
        if (auto conn = downstream()) {
            http::ResponseWriter writer(downstream_, ingress_.request_id, true);
            writer.writeChunk("data: " + err.dump() + "\n\n");
            writer.endStream();
            endstream_sent_ = true;
        }
    } else {
        http_status_ = status;
    }
    finish(Outcome::stream_interrupted);
}

void ProxySession::drainUpstream(const std::function<void(std::string_view)>& on_data,
                                const std::function<void()>& on_shutdown) {
    if (exchange_) exchange_->abortAndDrain(on_data, on_shutdown);
}

void ProxySession::feedMeterOnly(std::string_view data) {
    sse_parser_.feed(data, [this](http::SseEvent&& event) {
        if (stream_observer_) stream_observer_->process(event.data);
    });
}

void ProxySession::finalizeMeter(bool completed) {
    if (meter_ && !meter_finalized_) {
        meter_->finish(completed);
        meter_finalized_ = true;
    }
}

void ProxySession::failAttempt(int status, std::string message, bool close) {
    if (finished_) return;
    if (meter_) {
        const auto& usage = meter_->value();
        if (!usage.prompt && !usage.completion && !usage.cached && !usage.prompt_computed) {
            meter_->markRejected();
        }
    }
    std::string type = status == 504 ? "upstream_timeout" : "upstream_error";
    writeErrorResponse(status, proto::errorBody(ingress_.protocol, message, type), close);
    finish(Outcome::upstream_error);
}

void ProxySession::writeErrorResponse(int status, const std::string& body, bool close) {
    auto conn = downstream();
    if (!conn || !conn->connected()) return;
    http_status_ = status;
    http::Headers headers;
    if (target_) headers.emplace_back("x-gateway-upstream", target_->name);
    headers.emplace_back("x-gateway-attempts", std::to_string(attempts_));
    http::ResponseWriter writer(downstream_, ingress_.request_id, !close);
    writer.writeFull(status, headers, "application/json", body);
    if (close) conn->shutdown();
}

void ProxySession::cancel(std::string reason) {
    if (finished_) return;
    cancelled_ = true;
    cancel_reason_ = std::move(reason);
    auto start = nowSteady();
    // 取消延迟只统计到向上游发出 FIN 为止（排空已到达的数据不计入）
    auto cancel_latency = std::chrono::microseconds{0};
    auto mark_latency = [&] {
        cancel_latency = std::chrono::duration_cast<std::chrono::microseconds>(nowSteady() - start);
    };
    switch (stage_) {
        case Stage::acquiring:
            if (meter_) meter_->markRejected();
            if (acquire_handle_) acquire_handle_->cancel();
            mark_latency();
            break;
        case Stage::await_head:
        case Stage::await_first_event:
        case Stage::streaming:
            if (exchange_) {
                drainUpstream([this](std::string_view data) { feedMeterOnly(data); }, mark_latency);
            } else {
                mark_latency();
            }
            break;
        case Stage::backoff:
        case Stage::admitting:
        case Stage::done:
            mark_latency();
            break;
    }
    SPDLOG_DEBUG("proxy: cancelled request_id={} stage={} reason={} cancel_latency_us={}", ingress_.request_id,
                 toString(stage_), cancel_reason_, cancel_latency.count());
    finalizeMeter(false);
    finish(Outcome::cancelled);
}

void ProxySession::onDownstreamHighWater(std::size_t mark) {
    if (finished_ || !exchange_ || !committed_ || backpressure_paused_) return;
    backpressure_paused_ = true;
    backpressure_pauses_ += 1;
    pause_start_ = nowSteady();
    exchange_->pauseReading();
    startStallTimer();
    SPDLOG_DEBUG("proxy: backpressure pause request_id={} output={} (pause #{})", ingress_.request_id, mark,
                 backpressure_pauses_);
}

void ProxySession::onDownstreamWriteComplete() {
    if (finished_ || !exchange_ || !backpressure_paused_) return;
    backpressure_paused_ = false;
    backpressure_total_ += nowSteady() - pause_start_;
    if (stall_timer_.valid()) {
        if (auto conn = downstream()) conn->getLoop()->cancel(stall_timer_);
        stall_timer_ = net::TimerId{};
    }
    exchange_->resumeReading();
    SPDLOG_DEBUG("proxy: backpressure resume request_id={}", ingress_.request_id);
}

void ProxySession::finish(Outcome outcome) {
    if (finished_) return;
    finished_ = true;
    stage_ = Stage::done;
    outcome_ = outcome;
    cancelTimers();

    auto conn = downstream();
    if (exchange_) {
        bool can_release = upstream_reusable_ && !cancelled_;
        if (can_release && conn) {
            auto* pool_ptr = std::any_cast<std::shared_ptr<http::UpstreamPool>>(&conn->getLoop()->context());
            if (pool_ptr != nullptr) {
                (*pool_ptr)->release(exchange_->lease());
            } else {
                exchange_->forceClose();
            }
        } else {
            exchange_->forceClose();
        }
    }

    finalizeMeter(stream_completed_);
    logRecord();

    if (conn) {
        if (auto* state = http::HttpServer::stateOf(conn)) {
            state->session.reset();
        }
        server_.finishRequest(conn);
    }
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
