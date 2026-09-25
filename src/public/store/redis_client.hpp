#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/noncopyable.hpp"
#include "net/connector.hpp"
#include "net/event_loop.hpp"
#include "net/resolver.hpp"
#include "net/tcp_connection.hpp"
#include "store/resp.hpp"

namespace gw::store {

struct RedisOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string password;
    std::chrono::milliseconds command_timeout{50};
    std::chrono::milliseconds connect_timeout{3000};
};

enum class RedisErrorKind { connection, timeout, cancelled };
struct RedisError {
    RedisErrorKind kind = RedisErrorKind::connection;
    std::string message;
};

// 自研 RESP2 客户端：非阻塞、挂在 EventLoop 上，断线自动重连（退避 100ms → 5s）。
// 命令按发送顺序对应回复（FIFO），支持流水线（不等回复就连续发送）。
// 每个 io loop 一个实例，会话只用本 loop 的连接，不跨线程。
class RedisClient : noncopyable, public std::enable_shared_from_this<RedisClient> {
public:
    using Reply = std::expected<RespValue, RedisError>;
    using ReplyHandler = std::function<void(Reply)>;

    RedisClient(net::EventLoop* loop, net::Resolver& resolver, RedisOptions options);
    ~RedisClient();

    void connect();  // 异步建连（+AUTH）；失败自动重连
    bool connected() const { return ready_.load(std::memory_order_relaxed); }
    // 回调在本 loop 上执行。未连接时命令排队（带超时），超时后该连接上所有待回复命令失败并重连
    void command(std::vector<std::string> args, ReplyHandler cb);

private:
    struct Pending {
        std::vector<std::string> args;
        ReplyHandler cb;
        net::TimerId timer;
    };

    void doConnect(int attempt);
    void onConnected(int fd, net::InetAddress peer, int attempt);
    void onConnectFailed(net::ConnectError error, int attempt);
    void onReady();
    void flushQueue();
    void sendNow(const std::vector<std::string>& args);
    void armTimer(Pending& pending);
    void onMessage(const net::TcpConnectionPtr& conn, net::Buffer* buf, gw::Timestamp);
    void onCommandTimeout();
    void failAll(RedisError error);
    void scheduleReconnect(int attempt);
    void closeConnection();

    net::EventLoop* loop_;
    net::Resolver* resolver_;
    RedisOptions options_;
    std::shared_ptr<net::Connector> connector_;
    std::shared_ptr<net::TcpConnection> conn_;
    RespParser parser_;
    std::deque<Pending> pending_;  // FIFO：已发送未回复 + 已排队未发送
    std::atomic<bool> ready_{false};
    bool connecting_ = false;
    int backoff_attempt_ = 0;
};

}  // namespace gw::store
