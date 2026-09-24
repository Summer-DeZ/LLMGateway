#include "net/tcp_server.hpp"

#include <cassert>

#include "base/log.hpp"
#include "net/event_loop.hpp"
#include "net/socket.hpp"

namespace gw::net {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listen_addr, std::string name, bool reuse_port)
    : loop_(loop),
      name_(std::move(name)),
      listen_address_(listen_addr.toIpPort()),
      acceptor_(std::make_unique<Acceptor>(loop, listen_addr, reuse_port)),
      thread_pool_(std::make_unique<EventLoopThreadPool>(loop, name_)),
      started_(false) {
    acceptor_->setNewConnectionCallback([this](int sockfd, const InetAddress& peer) { newConnection(sockfd, peer); });
}

TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    for (auto& [name, conn] : connections_) {
        TcpConnectionPtr c(conn);
        conn.reset();
        c->getLoop()->runInLoop([c] { c->connectDestroyed(); });
    }
}

void TcpServer::start() {
    if (started_) return;
    started_ = true;
    thread_pool_->start(thread_init_callback_);
    assert(!acceptor_->listening());
    loop_->runInLoop([this] { acceptor_->listen(); });
}

void TcpServer::stopAccepting() {
    loop_->runInLoop([this] {
        if (acceptor_->listening()) acceptor_->stop();
    });
}

void TcpServer::newConnection(int sockfd, const InetAddress& peer_addr) {
    loop_->assertInLoopThread();
    EventLoop* io_loop = thread_pool_->getNextLoop();
    std::string conn_name = name_ + "#" + std::to_string(next_conn_id_.fetch_add(1));

    InetAddress local_addr;
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&local), &len) == 0) {
        if (local.ss_family == AF_INET) {
            local_addr = InetAddress(*reinterpret_cast<struct sockaddr_in*>(&local));
        } else if (local.ss_family == AF_INET6) {
            local_addr = InetAddress(*reinterpret_cast<struct sockaddr_in6*>(&local));
        }
    }

    auto transport = std::make_unique<PlainTransport>(sockfd);
    TcpConnectionPtr conn(new TcpConnection(io_loop, conn_name, sockfd, local_addr, peer_addr, std::move(transport)));
    connections_[conn_name] = conn;
    conn->setConnectionCallback(connection_callback_);
    conn->setMessageCallback(message_callback_);
    conn->setWriteCompleteCallback(write_complete_callback_);
    conn->setCloseCallback([this](const TcpConnectionPtr& c) { removeConnection(c); });
    conn->setTcpNoDelay(true);
    // 限制内核发送缓冲，让用户态高水位（背压）能及时生效，同时约束每连接内核内存
    conn->setSendBufferSize(64 * 1024);
    if (high_water_mark_callback_) {
        conn->setHighWaterMarkCallback(high_water_mark_callback_, high_water_mark_);
    }

    io_loop->runInLoop([conn] { conn->connectEstablished(); });
    SPDLOG_DEBUG("new connection {} from {} on loop {}", conn_name, peer_addr.toIpPort(),
                 static_cast<const void*>(io_loop));
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    loop_->runInLoop([this, conn] { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    connections_.erase(conn->name());
    EventLoop* io_loop = conn->getLoop();
    io_loop->queueInLoop([conn] { conn->connectDestroyed(); });
}

void TcpServer::forEachConnection(const std::function<void(const TcpConnectionPtr&)>& fn) {
    loop_->assertInLoopThread();
    for (auto& [name, conn] : connections_) {
        EventLoop* io_loop = conn->getLoop();
        io_loop->runInLoop([conn, fn] { fn(conn); });
    }
}

}  // namespace gw::net
