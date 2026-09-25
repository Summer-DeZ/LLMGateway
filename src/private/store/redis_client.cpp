#include "store/redis_client.hpp"

#include <unistd.h>

#include "base/log.hpp"

namespace gw::store {

namespace {
constexpr int kMaxBackoffAttempt = 6;  // 100ms × 2^5 = 3.2s → 封顶 5s
}  // namespace

RedisClient::RedisClient(net::EventLoop* loop, net::Resolver& resolver, RedisOptions options)
    : loop_(loop), resolver_(&resolver), options_(std::move(options)) {}

RedisClient::~RedisClient() { closeConnection(); }

void RedisClient::connect() {
    doConnect(0);
}

void RedisClient::doConnect(int attempt) {
    if (connecting_ || conn_) return;
    connecting_ = true;
    backoff_attempt_ = attempt;
    net::InetAddress addr(options_.host, options_.port);
    connector_ = std::make_shared<net::Connector>(loop_, std::vector<net::InetAddress>{addr},
                                                  options_.connect_timeout);
    std::weak_ptr<RedisClient> weak = shared_from_this();
    connector_->start(
        [weak, attempt](int fd, net::InetAddress peer) {
            if (auto self = weak.lock()) self->onConnected(fd, peer, attempt);
            else ::close(fd);
        },
        [weak, attempt](net::ConnectError error) {
            if (auto self = weak.lock()) self->onConnectFailed(std::move(error), attempt);
        });
}

void RedisClient::onConnected(int fd, net::InetAddress peer, int attempt) {
    (void)attempt;
    connecting_ = false;
    connector_.reset();
    net::InetAddress local;
    struct sockaddr_storage local_storage {};
    socklen_t local_len = sizeof(local_storage);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&local_storage), &local_len) == 0) {
        if (local_storage.ss_family == AF_INET) {
            local = net::InetAddress(*reinterpret_cast<struct sockaddr_in*>(&local_storage));
        } else if (local_storage.ss_family == AF_INET6) {
            local = net::InetAddress(*reinterpret_cast<struct sockaddr_in6*>(&local_storage));
        }
    }
    conn_ = std::make_shared<net::TcpConnection>(loop_, "redis#" + options_.host + ":" + std::to_string(options_.port),
                                                 fd, local, peer, std::make_unique<net::PlainTransport>(fd));
    conn_->setTcpNoDelay(true);
    std::weak_ptr<RedisClient> weak = shared_from_this();
    conn_->setMessageCallback([weak](const net::TcpConnectionPtr& c, net::Buffer* buf, gw::Timestamp t) {
        if (auto self = weak.lock()) self->onMessage(c, buf, t);
    });
    conn_->setCloseCallback([weak](const net::TcpConnectionPtr& c) {
        if (auto self = weak.lock()) self->scheduleReconnect(self->backoff_attempt_ + 1);
        (void)c;
    });
    conn_->connectEstablished();
    onReady();
}

void RedisClient::onConnectFailed(net::ConnectError error, int attempt) {
    connecting_ = false;
    connector_.reset();
    if (error.kind == net::ConnectErrorKind::cancelled) return;
    // 排队中的命令先失败（连接始终建立不起来，不能让调用方一直等）
    failAll(RedisError{RedisErrorKind::connection, "connect failed: " + error.message});
    scheduleReconnect(attempt + 1);
}

void RedisClient::onReady() {
    ready_.store(true, std::memory_order_relaxed);
    backoff_attempt_ = 0;
    if (!options_.password.empty()) {
        // AUTH 与后续命令共用 FIFO：回复顺序自然正确；AUTH 失败时后续命令会收到 NOAUTH 错误回复
        sendNow({"AUTH", options_.password});
        pending_.push_back(Pending{
            {"AUTH", options_.password},
            [weak = weak_from_this()](Reply r) {
                if (!weak.lock()) return;
                if (const std::string* err = store::asError(*r)) {
                    SPDLOG_WARN("redis auth failed: {}", *err);
                }
            },
            net::TimerId{}});
        armTimer(pending_.back());
    }
    flushQueue();
}

void RedisClient::flushQueue() {
    // 补发排队中的命令（保持 FIFO：无 timer 的都是尚未发送的）
    std::vector<std::vector<std::string>> to_send;
    for (auto& p : pending_) {
        if (!p.timer.valid()) to_send.push_back(p.args);
    }
    for (auto& args : to_send) sendNow(args);
    for (auto& p : pending_) armTimer(p);
}

void RedisClient::command(std::vector<std::string> args, ReplyHandler cb) {
    loop_->assertInLoopThread();
    Pending p{std::move(args), std::move(cb), net::TimerId{}};
    if (!ready_.load(std::memory_order_relaxed)) {
        // 未连接：排队等重连成功后补发；超时同样生效
        pending_.push_back(std::move(p));
        armTimer(pending_.back());
        return;
    }
    sendNow(p.args);
    pending_.push_back(std::move(p));
    armTimer(pending_.back());
}

void RedisClient::armTimer(Pending& pending) {
    if (pending.timer.valid()) return;
    auto weak = weak_from_this();
    pending.timer = loop_->runAfter(options_.command_timeout, [weak] {
        if (auto self = weak.lock()) self->onCommandTimeout();
    });
}

void RedisClient::onCommandTimeout() {
    // RESP 没有请求 ID，丢一个回复后无法再对齐：超时后本连接上所有待回复命令失败并重连
    closeConnection();
    failAll(RedisError{RedisErrorKind::timeout, "command timeout"});
    scheduleReconnect(0);
}

void RedisClient::sendNow(const std::vector<std::string>& args) {
    if (!conn_) return;
    conn_->send(store::encodeCommand(args));
}

void RedisClient::onMessage(const net::TcpConnectionPtr& conn, net::Buffer* buf, gw::Timestamp) {
    (void)conn;
    while (buf->readableBytes() > 0 && !pending_.empty()) {
        RespValue value;
        std::string error;
        std::size_t consumed = 0;
        auto result = parser_.parse(std::string_view(buf->peek(), buf->readableBytes()), consumed, value, error);
        if (result == RespParser::Result::need_more) return;
        if (result == RespParser::Result::error) {
            // 流失步：丢弃连接上所有待回复命令并重连
            SPDLOG_WARN("redis reply parse error: {}", error);
            closeConnection();
            failAll(RedisError{RedisErrorKind::connection, "reply parse error: " + error});
            scheduleReconnect(0);
            return;
        }
        buf->retrieve(consumed);
        Pending p = std::move(pending_.front());
        pending_.pop_front();
        if (p.timer.valid()) loop_->cancel(p.timer);
        if (p.cb) p.cb(Reply(std::move(value)));
        (void)0;
    }
}

void RedisClient::failAll(RedisError error) {
    auto pending = std::move(pending_);
    pending_.clear();
    while (!pending.empty()) {
        Pending p = std::move(pending.front());
        pending.pop_front();
        if (p.timer.valid()) loop_->cancel(p.timer);
        if (p.cb) p.cb(std::unexpected(error));
    }
}

void RedisClient::scheduleReconnect(int attempt) {
    closeConnection();
    if (attempt > kMaxBackoffAttempt) attempt = kMaxBackoffAttempt;
    auto delay = std::chrono::milliseconds(100 * (1 << attempt));
    if (delay > std::chrono::seconds(5)) delay = std::chrono::seconds(5);
    auto weak = weak_from_this();
    loop_->runAfter(delay, [weak, attempt] {
        if (auto self = weak.lock()) self->doConnect(attempt);
    });
}

void RedisClient::closeConnection() {
    ready_.store(false, std::memory_order_relaxed);
    if (conn_) {
        conn_->setCloseCallback(nullptr);
        conn_->forceClose();
        conn_.reset();
    }
    parser_.reset();
}

}  // namespace gw::store
