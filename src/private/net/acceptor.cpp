#include "net/acceptor.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "base/log.hpp"
#include "net/event_loop.hpp"

namespace gw::net {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listen_addr, bool reuse_port)
    : loop_(loop),
      accept_socket_(Socket::createNonblocking(listen_addr.family())),
      accept_channel_(loop, accept_socket_.fd()),
      listening_(false),
      idle_fd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
    accept_socket_.setReuseAddr(true);
    accept_socket_.setReusePort(reuse_port);
    accept_socket_.bindAddress(listen_addr);
    accept_channel_.setReadCallback([this] { handleRead(); });
}

Acceptor::~Acceptor() {
    accept_channel_.disableAll();
    accept_channel_.remove();
    if (idle_fd_ >= 0) ::close(idle_fd_);
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    listening_ = true;
    accept_socket_.listen();
    accept_channel_.enableReading();
}

void Acceptor::stop() {
    loop_->assertInLoopThread();
    listening_ = false;
    accept_channel_.disableAll();
}

void Acceptor::handleRead() {
    loop_->assertInLoopThread();
    InetAddress peer;
    int saved_errno = 0;
    int connfd = accept_socket_.accept(&peer, &saved_errno);
    if (connfd >= 0) {
        if (new_connection_callback_) {
            new_connection_callback_(connfd, peer);
        } else {
            ::close(connfd);
        }
        return;
    }

    if (saved_errno == EMFILE || saved_errno == ENFILE) {
        // 备用 fd 技巧：先关掉 /dev/null 腾一个 fd，accept 后立刻关闭，再重新打开
        ::close(idle_fd_);
        idle_fd_ = -1;
        int tmp = accept_socket_.accept(&peer, &saved_errno);
        if (tmp >= 0) ::close(tmp);
        idle_fd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        SPDLOG_ERROR("accept: {} (hit fd limit), connection dropped", std::strerror(saved_errno));
        return;
    }
    if (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK && saved_errno != EINTR &&
        saved_errno != ECONNABORTED) {
        SPDLOG_ERROR("accept failed: {}", std::strerror(saved_errno));
    }
}

}  // namespace gw::net
