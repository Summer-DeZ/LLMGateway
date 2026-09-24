#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "gateway/config.hpp"
#include "gateway/context.hpp"
#include "gateway/usage_meter.hpp"
#include "http/http_request.hpp"
#include "http/http_server.hpp"
#include "http/sse.hpp"
#include "http/upstream_client.hpp"
#include "net/tcp_connection.hpp"
#include "proto/neutral.hpp"
#include "proto/openai.hpp"

namespace gw::gateway {

// 一次代理请求的状态机；只在所属下游连接的 io loop 线程上运行，不加锁。
class ProxySession : public std::enable_shared_from_this<ProxySession>, noncopyable {
public:
    enum class Stage { admitting, acquiring, await_head, await_first_event, streaming, backoff, done };
    enum class Outcome {
        ok,
        cache_hit,
        upstream_error,
        rate_limited,
        bad_request,
        unauthorized,
        cancelled,
        downstream_stalled,
        stream_interrupted,
        unavailable,
        quota_exceeded,
        quota_truncated
    };

    struct Ingress {
        proto::Protocol protocol = proto::Protocol::openai;
        std::string request_id;
        Timestamp received_at;
        http::HttpRequest http;
        const UserConfig* user = nullptr;
    };

    ProxySession(GatewayContext& context, http::HttpServer& server, net::TcpConnectionPtr conn, Ingress ingress);
    ~ProxySession();

    void start();
    void cancel(std::string reason);
    void onDownstreamHighWater(std::size_t mark);
    void onDownstreamWriteComplete();

    bool finished() const { return finished_; }
    Outcome outcome() const { return outcome_; }
    Stage stage() const { return stage_; }
    int httpStatus() const { return http_status_; }
    const std::string& requestId() const { return ingress_.request_id; }

private:
    void admit();
    // allow_idle=false：复用连接首字节前失败后的重发，一定新建连接
    void startAttempt(bool allow_idle = true);
    void onAcquire(std::expected<http::Lease, http::AcquireError> result);
    void onUpstreamHead(int status, const http::Headers& headers);
    void onUpstreamBody(std::string_view data);
    void onUpstreamComplete(bool reusable);
    void onUpstreamError(http::UpstreamIoError error);
    void handleSseEvent(http::SseEvent&& event);
    void handleNonStreamBody();
    void commitStream();
    // 结束当前尝试：SHUT_WR → 读出已到达的字节只喂计量 → 关闭（proxy-session.md §5.2）
    void abortUpstream(const std::function<void()>& on_shutdown = {});

    void failAttempt(int status, std::string message, bool close);
    void failCommitted(int status, const std::string& message);
    void finalizeMeter(bool completed);
    void writeErrorResponse(int status, const std::string& body, bool close);
    void finish(Outcome outcome);

    void startTtftTimer();
    void startIdleTimer();
    void startStallTimer();
    void startTotalTimer();
    void startPrecommitTimer();
    void cancelTimer(net::TimerId& id);
    void cancelTimers();

    net::TcpConnectionPtr downstream();
    void logRecord();

    GatewayContext& context_;
    http::HttpServer& server_;
    std::weak_ptr<net::TcpConnection> downstream_;
    Ingress ingress_;

    proto::OpenAiRequestMeta meta_;
    nlohmann::json request_json_;
    const ModelConfig* route_ = nullptr;
    const UpstreamConfig* target_ = nullptr;
    http::UpstreamKey key_;

    std::shared_ptr<http::AcquireHandle> acquire_handle_;
    std::shared_ptr<http::UpstreamExchange> exchange_;
    std::unique_ptr<UsageMeter> meter_;
    std::unique_ptr<proto::OpenAiStreamObserver> stream_observer_;
    http::SseParser sse_parser_;
    std::string nonstream_body_;
    int upstream_status_ = 0;
    http::Headers upstream_headers_;
    int attempts_ = 1;
    bool upstream_reusable_ = false;
    bool reuse_retried_ = false;
    bool in_sse_feed_ = false;

    Stage stage_ = Stage::admitting;
    Outcome outcome_ = Outcome::ok;
    bool finished_ = false;
    bool committed_ = false;
    bool cancelled_ = false;
    std::string cancel_reason_;
    int http_status_ = 0;
    Duration ttft_{};
    Duration upstream_ttfb_{};
    bool first_progress_seen_ = false;
    bool stream_completed_ = false;
    bool meter_finalized_ = false;
    bool endstream_sent_ = false;
    Timestamp request_written_{};

    bool backpressure_paused_ = false;
    int backpressure_pauses_ = 0;
    Duration backpressure_total_{};
    Timestamp pause_start_{};

    net::TimerId ttft_timer_;
    net::TimerId idle_timer_;
    net::TimerId stall_timer_;
    net::TimerId total_timer_;
    net::TimerId precommit_timer_;
};

const char* toString(ProxySession::Stage stage);
const char* toString(ProxySession::Outcome outcome);

}  // namespace gw::gateway
