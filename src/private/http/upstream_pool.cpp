#include "http/upstream_pool.hpp"

#include <unistd.h>

#include <algorithm>

#include "base/log.hpp"
#include "net/transport.hpp"

namespace gw::http {

using namespace std::chrono_literals;

void AcquireHandle::cancel() {
    cancelled_ = true;
    cancelConnector();
}

void AcquireHandle::cancelConnector() {
    if (auto connector = connector_.lock()) {
        connector->cancel();
    }
}

UpstreamPool::UpstreamPool(net::EventLoop* loop, net::Resolver* resolver, PoolOptions options)
    : loop_(loop), resolver_(resolver), options_(std::move(options)) {}

UpstreamPool::~UpstreamPool() {
    closeAll();
}

std::shared_ptr<net::TcpConnection> UpstreamPool::takeIdle(const UpstreamKey& key) {
    auto it = idle_.find(key);
    if (it == idle_.end() || it->second.empty()) return nullptr;
    while (!it->second.empty()) {
        IdleEntry entry = it->second.back();
        it->second.pop_back();
        idle_keys_.erase(entry.conn.get());
        auto idle_limit = options_.idle_timeout_for ? options_.idle_timeout_for(key) : options_.idle_timeout;
        if (nowSteady() - entry.last_used > idle_limit) {
            SPDLOG_DEBUG("upstream pool: closing expired idle connection {} -> {}", entry.conn->name(),
                         key.host + ":" + std::to_string(key.port));
            entry.conn->forceClose();
            continue;
        }
        if (!entry.conn->connected()) continue;
        return entry.conn;
    }
    return nullptr;
}

void UpstreamPool::putIdle(const UpstreamKey& key, const std::shared_ptr<net::TcpConnection>& conn) {
    auto& entries = idle_[key];
    if (static_cast<int>(entries.size()) >= options_.max_idle_per_host) {
        IdleEntry oldest = entries.front();
        entries.erase(entries.begin());
        idle_keys_.erase(oldest.conn.get());
        oldest.conn->forceClose();
    }
    conn->setMessageCallback([key](const net::TcpConnectionPtr& c, net::Buffer* buf, gw::Timestamp) {
        buf->retrieveAll();
        SPDLOG_DEBUG("upstream pool: unexpected data on idle connection {}", c->name());
        c->forceClose();
    });
    conn->setCloseCallback([this](const net::TcpConnectionPtr& c) { removeIdle(c); });
    conn->setConnectionCallback(nullptr);
    conn->setWriteCompleteCallback(nullptr);
    entries.push_back(IdleEntry{conn, nowSteady()});
    idle_keys_[conn.get()] = key;
    SPDLOG_DEBUG("upstream pool: idle {} -> {} (idle_count={})", conn->name(),
                 key.host + ":" + std::to_string(key.port), entries.size());
}

void UpstreamPool::removeIdle(const std::shared_ptr<net::TcpConnection>& conn) {
    auto kit = idle_keys_.find(conn.get());
    if (kit == idle_keys_.end()) return;
    UpstreamKey key = kit->second;
    idle_keys_.erase(kit);
    auto it = idle_.find(key);
    if (it == idle_.end()) return;
    auto& entries = it->second;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const IdleEntry& e) { return e.conn.get() == conn.get(); }),
                  entries.end());
}

void UpstreamPool::release(Lease lease) {
    loop_->assertInLoopThread();
    if (!lease.conn || !lease.conn->connected()) return;
    putIdle(lease.key, lease.conn);
}

std::size_t UpstreamPool::idleCount() const {
    std::size_t total = 0;
    for (const auto& [key, entries] : idle_) total += entries.size();
    return total;
}

void UpstreamPool::closeAll() {
    std::vector<std::shared_ptr<net::TcpConnection>> conns;
    for (auto& [key, entries] : idle_) {
        for (auto& entry : entries) conns.push_back(entry.conn);
    }
    idle_.clear();
    idle_keys_.clear();
    for (auto& conn : conns) {
        conn->forceClose();
    }
}

std::shared_ptr<AcquireHandle> UpstreamPool::acquire(const UpstreamKey& key,
                                                     std::chrono::milliseconds connect_timeout,
                                                     std::function<void(std::expected<Lease, AcquireError>)> cb,
                                                     bool allow_idle) {
    loop_->assertInLoopThread();
    pending_connectors_.erase(std::remove_if(pending_connectors_.begin(), pending_connectors_.end(),
                                             [](const std::shared_ptr<net::Connector>& c) { return c->finished(); }),
                              pending_connectors_.end());
    auto handle = std::make_shared<AcquireHandle>();

    if (allow_idle) {
        if (auto conn = takeIdle(key)) {
            Lease lease;
            lease.conn = conn;
            lease.key = key;
            lease.reused = true;
            cb(std::expected<Lease, AcquireError>(std::move(lease)));
            return handle;
        }
    }

    resolver_->resolve(key.host, key.port, loop_,
                       [this, key, connect_timeout, handle, cb = std::move(cb)](net::Resolver::Result result) mutable {
                           if (handle->cancelled()) {
                               cb(std::unexpected(AcquireError{AcquireError::Kind::cancelled, "cancelled"}));
                               return;
                           }
                           if (!result) {
                               cb(std::unexpected(AcquireError{AcquireError::Kind::resolve, result.error()}));
                               return;
                           }
                           startConnect(key, connect_timeout, handle, std::move(cb), std::move(*result));
                       });
    return handle;
}

void UpstreamPool::startConnect(const UpstreamKey& key, std::chrono::milliseconds connect_timeout,
                                const std::shared_ptr<AcquireHandle>& handle,
                                std::function<void(std::expected<Lease, AcquireError>)> cb,
                                std::vector<net::InetAddress> addrs) {
    Timestamp start = nowSteady();
    auto connector = std::make_shared<net::Connector>(loop_, std::move(addrs), connect_timeout);
    handle->connector_ = connector;
    pending_connectors_.push_back(connector);

    // 两个 lambda 都持有 cb，必须共享同一个可调用对象（否则先构造的会把 cb 移空）
    auto shared_cb = std::make_shared<std::function<void(std::expected<Lease, AcquireError>)>>(std::move(cb));
    std::weak_ptr<net::Connector> weak_connector = connector;
    auto forget_connector = [this, weak_connector] {
        if (auto c = weak_connector.lock()) {
            pending_connectors_.erase(std::remove(pending_connectors_.begin(), pending_connectors_.end(), c),
                                      pending_connectors_.end());
        }
    };

    connector->start(
        [this, key, handle, shared_cb, start, forget_connector](int fd, net::InetAddress peer) {
            forget_connector();
            if (handle->cancelled()) {
                ::close(fd);
                (*shared_cb)(std::unexpected(AcquireError{AcquireError::Kind::cancelled, "cancelled"}));
                return;
            }

            net::InetAddress local;
            struct sockaddr_storage local_storage;
            socklen_t local_len = sizeof(local_storage);
            if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&local_storage), &local_len) == 0) {
                if (local_storage.ss_family == AF_INET) {
                    local = net::InetAddress(*reinterpret_cast<struct sockaddr_in*>(&local_storage));
                } else if (local_storage.ss_family == AF_INET6) {
                    local = net::InetAddress(*reinterpret_cast<struct sockaddr_in6*>(&local_storage));
                }
            }

            auto conn = std::make_shared<net::TcpConnection>(loop_, "up#" + key.host + ":" + std::to_string(key.port),
                                                             fd, local, peer,
                                                             std::make_unique<net::PlainTransport>(fd));
            conn->setTcpNoDelay(true);
            conn->connectEstablished();

            Lease lease;
            lease.conn = conn;
            lease.key = key;
            lease.reused = false;
            lease.connect_time = nowSteady() - start;
            (*shared_cb)(std::expected<Lease, AcquireError>(std::move(lease)));
        },
        [shared_cb, forget_connector](net::ConnectError error) {
            forget_connector();
            AcquireError::Kind kind = AcquireError::Kind::connect_refused;
            switch (error.kind) {
                case net::ConnectErrorKind::timeout: kind = AcquireError::Kind::connect_timeout; break;
                case net::ConnectErrorKind::cancelled: kind = AcquireError::Kind::cancelled; break;
                case net::ConnectErrorKind::resolve: kind = AcquireError::Kind::resolve; break;
                case net::ConnectErrorKind::refused:
                case net::ConnectErrorKind::unreachable: kind = AcquireError::Kind::connect_refused; break;
            }
            (*shared_cb)(std::unexpected(AcquireError{kind, error.message}));
        });
}

}  // namespace gw::http
