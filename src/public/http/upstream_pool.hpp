#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "net/connector.hpp"
#include "net/event_loop.hpp"
#include "net/resolver.hpp"
#include "net/tcp_connection.hpp"

namespace gw::net {
class TlsContext;
}

namespace gw::http {

struct UpstreamKey {
    bool tls = false;
    std::string host;
    std::uint16_t port = 80;
    auto operator<=>(const UpstreamKey&) const = default;
};

struct AcquireError {
    enum class Kind { resolve, connect_refused, connect_timeout, tls_verify, tls_handshake, cancelled };
    Kind kind = Kind::connect_refused;
    std::string message;
};

struct PoolOptions {
    int max_idle_per_host = 16;
    std::chrono::seconds idle_timeout{4};
    // 每个上游可覆盖空闲超时（配置的 pool_idle_seconds）
    std::function<std::chrono::seconds(const UpstreamKey&)> idle_timeout_for;
    bool tls_verify_peer = true;
    std::string ca_file;
};

struct Lease {
    std::shared_ptr<net::TcpConnection> conn;
    UpstreamKey key;
    bool reused = false;
    Duration connect_time{};
    Duration tls_time{};
    bool tls_resumed = false;
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
    UpstreamPool(net::EventLoop* loop, net::Resolver* resolver, net::TlsContext* tls_context, PoolOptions options);
    ~UpstreamPool();

    // 回调在本 loop 上执行；返回的连接已完成握手、处于 kConnected
    std::shared_ptr<AcquireHandle> acquire(const UpstreamKey& key, std::chrono::milliseconds connect_timeout,
                                           std::function<void(std::expected<Lease, AcquireError>)> cb);
    void release(Lease lease);
    std::size_t idleCount() const;
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
    net::TlsContext* tls_context_;
    PoolOptions options_;
    std::map<UpstreamKey, std::vector<IdleEntry>> idle_;
    std::map<net::TcpConnection*, UpstreamKey> idle_keys_;
    std::vector<std::shared_ptr<net::Connector>> pending_connectors_;
};

}  // namespace gw::http
