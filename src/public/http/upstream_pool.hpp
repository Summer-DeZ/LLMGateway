#pragma once

#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "net/connector.hpp"
#include "net/event_loop.hpp"
#include "net/resolver.hpp"
#include "net/tcp_connection.hpp"

namespace gw::http {

struct UpstreamKey {
    std::string host;
    std::uint16_t port = 80;
    auto operator<=>(const UpstreamKey&) const = default;
};

struct AcquireError {
    enum class Kind { resolve, connect_refused, connect_timeout, cancelled };
    Kind kind = Kind::connect_refused;
    std::string message;
};

struct PoolOptions {
    int max_idle_per_host = 16;
    std::chrono::seconds idle_timeout{4};
    // 每个上游可覆盖空闲超时（配置的 pool_idle_seconds）
    std::function<std::chrono::seconds(const UpstreamKey&)> idle_timeout_for;
};

struct Lease {
    std::shared_ptr<net::TcpConnection> conn;
    UpstreamKey key;
    bool reused = false;
    Duration connect_time{};
};

class UpstreamPool;

class AcquireHandle : public std::enable_shared_from_this<AcquireHandle> {
public:
    void cancel();
    bool cancelled() const { return cancelled_; }

private:
    friend class UpstreamPool;
    void cancelConnector();

    std::weak_ptr<net::Connector> connector_;
    bool cancelled_ = false;
};

// 每 loop 一个；连接不跨 loop 复用
class UpstreamPool : noncopyable {
public:
    UpstreamPool(net::EventLoop* loop, net::Resolver* resolver, PoolOptions options);
    ~UpstreamPool();

    // 回调在本 loop 上执行，未取消的 acquire 恰好回调一次；AcquireHandle::cancel() 之后不再保证回调。
    // 返回的连接处于 kConnected。allow_idle=false 时跳过空闲连接、一定新建（复用连接首字节前失败后的重发用）
    std::shared_ptr<AcquireHandle> acquire(const UpstreamKey& key, std::chrono::milliseconds connect_timeout,
                                           std::function<void(std::expected<Lease, AcquireError>)> cb,
                                           bool allow_idle = true);
     void release(Lease lease);
    // 原子计数，可跨线程读取（主循环刷指标用）；池只在本 loop 上修改
    std::size_t idleCount() const;
    // 该池是否服务过这个 host:port（主循环刷指标用；任意线程可调）
    bool servesKey(const UpstreamKey& key) const;
    void closeAll();

private:
    struct IdleEntry {
        std::shared_ptr<net::TcpConnection> conn;
        Timestamp last_used;
    };

    std::shared_ptr<net::TcpConnection> takeIdle(const UpstreamKey& key);
    void putIdle(const UpstreamKey& key, const std::shared_ptr<net::TcpConnection>& conn);
    void removeIdle(const std::shared_ptr<net::TcpConnection>& conn);
    void startConnect(const UpstreamKey& key, std::chrono::milliseconds connect_timeout,
                      const std::shared_ptr<AcquireHandle>& handle,
                      std::function<void(std::expected<Lease, AcquireError>)> cb, std::vector<net::InetAddress> addrs);

    net::EventLoop* loop_;
    net::Resolver* resolver_;
    PoolOptions options_;
    std::map<UpstreamKey, std::vector<IdleEntry>> idle_;
    std::map<net::TcpConnection*, UpstreamKey> idle_keys_;
    std::vector<std::shared_ptr<net::Connector>> pending_connectors_;
    std::atomic<std::size_t> idle_total_{0};  // 与 idle_ 同步维护，供跨线程读取
    mutable std::mutex known_keys_mu_;
    std::set<UpstreamKey> known_keys_;        // 服务过的 host:port（putIdle/acquire 时登记）
};

}  // namespace gw::http
