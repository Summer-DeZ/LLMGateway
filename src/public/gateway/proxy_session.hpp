#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "gateway/circuit_breaker.hpp"
#include "gateway/config.hpp"
#include "gateway/context.hpp"
#include "gateway/quota.hpp"
#include "gateway/rate_limiter.hpp"
#include "gateway/request_record.hpp"
#include "gateway/response_cache.hpp"
#include "gateway/retry_policy.hpp"
#include "gateway/usage_meter.hpp"
#include "http/http_request.hpp"
#include "http/http_server.hpp"
#include "http/sse.hpp"
#include "http/upstream_client.hpp"
#include "net/tcp_connection.hpp"
#include "proto/neutral.hpp"
#include "proto/openai.hpp"
#include "proto/anthropic.hpp"
#include "proto/translator.hpp"

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
        std::shared_ptr<const UserConfig> user;
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
    // 准入 3a/3b：账本健康 + 配额（异步，状态 admitting）。完成后进入 proceedAdmission
    void checkLedgerAndQuota();
    void onQuotaChecked(std::int64_t used_day, std::int64_t used_month);
    void onQuotaFailed(const std::string& message);
    // 准入 4-7：限流 → 缓存 → 尝试计划 → Acquiring
    void proceedAdmission();
    // 准入第 5 步：响应缓存（policies.md §2）。命中则回放并 finish(cache_hit)
    bool checkCache();
    bool cacheableRequest() const;
    void replayCache();
    // 流式转发时同步暂存缓存体（超过单条上限则放弃）
    void stageCacheBytes(std::string_view framed);
    // 结束时按条件写入缓存（outcome ok / finish stop|length|tool_calls / usage_final / 大小限制）
    void writeCacheEntry();
    // 准入第 4 步：限流预留（policies.md §1.3）。预留不是消耗，结束时按真实值结算
    bool acquireRateTicket();
    http::Headers rateHeaders() const;
    // 尝试计划（proxy-session.md §6.2）：跳过熔断打开的目标；半开只允许一个探测
    bool selectTarget();
    // allow_idle=false：复用连接首字节前失败后的重发，一定新建连接
    // resend=true：复用连接首字节前断开后的同一次尝试重发（不计尝试次数、保留计量状态）
    void startAttempt(bool allow_idle = true, bool resend = false);
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
    // ===== G12：尝试结束的记账与重试/降级 =====
    enum class BreakerAction { none, success, failure };
    // 当前尝试结束：meter 定稿、追加 attempt_list_、breaker 记账（恰好一次）
    void finalizeAttempt(const char* result, BreakerAction breaker);
    // 上游 HTTP >= 400 的响应体处理（可重试则重试/降级，否则透传）
    void handleUpstreamErrorBody();
    // 尝试失败：按失败分类决定重试（同目标退避）或降级（换下一个目标）或耗尽后返回错误
    void attemptFailure(const FailureClass& cls, int http_status, std::string message, bool close,
                        std::optional<Duration> retry_after, std::string error_body = {}, bool stop_retry = false);
    void nextTarget();
    void finishAttemptsExhausted();
    void startBackoff(Duration delay);
    // 各尝试 consumed 之和；任一次未知 → nullopt（限流结算用）
    std::optional<std::int64_t> totalConsumed() const;
    // 配额判断用的跨尝试累计：{consumed 已知部分之和, projected 已知部分之和}
    std::pair<std::int64_t, std::int64_t> quotaTotals() const;
    void writeErrorResponse(int status, const std::string& body, bool close, const http::Headers& extra_headers = {});
    void finish(Outcome outcome);
    // ===== 配额（usage-ledger.md §5.4）=====
    store::RedisClient* redisClient() const;
    // meter 更新后调用：本地超额判断 + 增量上报
    void onQuotaMeterUpdate();
    void sendQuotaAdd(std::int64_t delta);
    // 流式超额截断：按取消路径停上游 + 按协议正常收尾下游（usage-ledger.md §6）
    void truncateForQuota();
    void rejectForQuota();
    // G09：汇总 RequestRecord → Metrics / AccessLog / RecentRequests / 慢请求日志 / 账本
    void observeFinish();
    const char* cancellationStage() const;

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
    bool count_tokens_ = false;
    proto::Protocol upstream_protocol_ = proto::Protocol::openai;
    nlohmann::json upstream_request_;
    nlohmann::json request_json_;
    const ModelConfig* route_ = nullptr;
    const UpstreamConfig* target_ = nullptr;
    http::UpstreamKey key_;
    std::shared_ptr<RateTicket> rate_ticket_;

    // 尝试计划（G12）：目标链下标、目标内尝试次数（1-based）、总尝试次数
    std::size_t target_index_ = 0;
    int attempt_in_target_ = 1;
    int attempts_ = 0;
    bool fallback_ = false;
    // 最后一次失败的上下文（耗尽后决定返回给客户端的错误）
    int last_failure_status_ = 0;
    std::string last_failure_message_;
    std::string last_failure_body_;
    std::optional<Duration> last_failure_retry_after_;
    bool last_failure_timeout_ = false;
    bool last_failure_close_ = false;
    bool attempt_recorded_ = false;
    int attempt_epoch_ = 0;  // 每次尝试 +1：旧 exchange 的迟到回调按 epoch 丢弃
    Timestamp precommit_deadline_{};

    std::shared_ptr<http::AcquireHandle> acquire_handle_;
    std::shared_ptr<http::UpstreamExchange> exchange_;
    std::unique_ptr<UsageMeter> meter_;
    std::unique_ptr<proto::OpenAiStreamObserver> stream_observer_;
    proto::AnthropicStreamState anthropic_state_;
    std::unique_ptr<proto::StreamDecoder> decoder_;
    std::unique_ptr<proto::StreamEncoder> encoder_;
    http::SseParser sse_parser_;
    std::string nonstream_body_;
    int upstream_status_ = 0;
    http::Headers upstream_headers_;
    bool upstream_reusable_ = false;
    bool reuse_retried_ = false;
    bool in_sse_feed_ = false;

    Stage stage_ = Stage::admitting;
    Outcome outcome_ = Outcome::ok;
    bool finished_ = false;
    bool committed_ = false;
    bool cancelled_ = false;
    std::string cancel_reason_;
    std::string error_;
    std::int64_t cancel_latency_us_ = 0;
    int http_status_ = 0;
    Duration ttft_{};
    Duration upstream_ttfb_{};
    bool first_progress_seen_ = false;
    bool stream_completed_ = false;
    bool meter_finalized_ = false;
    bool endstream_sent_ = false;
    Timestamp request_written_{};
    std::int64_t wall_ts_ = 0;  // 请求收齐时刻（UTC 毫秒），TTFT/延迟的起点与 ts 字段
    std::vector<AttemptRecord> attempt_list_;
    std::optional<std::int64_t> last_connect_ms_;
    std::optional<bool> last_conn_reused_;

    bool backpressure_paused_ = false;
    int backpressure_pauses_ = 0;
    Duration backpressure_total_{};
    Timestamp pause_start_{};

    std::optional<QuotaView> quota_view_;
    bool quota_truncated_ = false;

    // 响应缓存
    std::vector<std::uint8_t> cache_key_;
    ResponseCache::EntryPtr cache_entry_;
    bool cache_hit_ = false;
    bool cache_miss_ = false;
    std::string cache_staging_;
    bool cache_overflow_ = false;

    net::TimerId ttft_timer_;
    net::TimerId idle_timer_;
    net::TimerId stall_timer_;
    net::TimerId total_timer_;
    net::TimerId precommit_timer_;
    net::TimerId backoff_timer_;
};

const char* toString(ProxySession::Stage stage);
const char* toString(ProxySession::Outcome outcome);

}  // namespace gw::gateway
