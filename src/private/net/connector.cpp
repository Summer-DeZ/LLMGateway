#include "net/connector.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "base/log.hpp"
#include "net/event_loop.hpp"
#include "net/socket.hpp"

namespace gw::net {

Connector::Connector(EventLoop* loop, std::vector<InetAddress> addrs, std::chrono::milliseconds timeout)
    : loop_(loop), addrs_(std::move(addrs)), timeout_(timeout) {}

Connector::~Connector() {
    if (channel_) {
        channel_->disableAll();
        channel_->remove();
        channel_.reset();
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// Channel 可能在 handleEvent 内被移除；reset 必须延后，否则触发 Channel 析构断言
void Connector::removeAndResetChannel() {
    if (!channel_) return;
    channel_->disableAll();
    channel_->remove();
    auto channel = std::shared_ptr<Channel>(std::move(channel_));
    loop_->queueInLoop([channel]() mutable { channel.reset(); });
}

void Connector::start(std::function<void(int, InetAddress)> on_connected,
                      std::function<void(ConnectError)> on_failed) {
    on_connected_ = std::move(on_connected);
    on_failed_ = std::move(on_failed);
    loop_->assertInLoopThread();
    SPDLOG_DEBUG("connector: start timeout={}ms", timeout_.count());
    if (addrs_.empty()) {
        finishFailure(ConnectErrorKind::resolve, "no addresses");
        return;
    }
    std::weak_ptr<Connector> weak = shared_from_this();
    timeout_timer_ = loop_->runAfter(timeout_, [weak] {
        auto self = weak.lock();
        if (!self || self->finished_) return;
        self->finishFailure(ConnectErrorKind::timeout, "connect timeout");
    });
    connectNext();
}

void Connector::connectNext() {
    if (cancelled_ || finished_) return;
    if (next_ >= addrs_.size()) {
        finishFailure(last_error_.kind, last_error_.message.empty() ? "connect failed" : last_error_.message);
        return;
    }
    InetAddress peer = addrs_[next_++];
    int fd = Socket::createNonblocking(peer.family());
    if (fd < 0) {
        last_error_ = {ConnectErrorKind::unreachable, std::string("socket: ") + std::strerror(errno)};
        connectNext();
        return;
    }
    int ret = ::connect(fd, peer.sockaddr(), peer.sockaddrLen());
    if (ret == 0) {
        fd_ = fd;
        finishSuccess();
        return;
    }
    if (errno != EINPROGRESS) {
        last_error_ = {ConnectErrorKind::refused, std::string("connect: ") + std::strerror(errno)};
        ::close(fd);
        connectNext();
        return;
    }

    fd_ = fd;
    channel_ = std::make_unique<Channel>(loop_, fd);
    channel_->setWriteCallback([this] { handleWrite(); });
    channel_->setErrorCallback([this] { handleWrite(); });
    channel_->enableWriting();
}

void Connector::handleWrite() {
    if (finished_ || cancelled_) return;
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        err = errno;
    }
    if (err != 0) {
        last_error_ = {err == ECONNREFUSED ? ConnectErrorKind::refused : ConnectErrorKind::unreachable,
                       std::string("connect: ") + std::strerror(err)};
        removeAndResetChannel();
        ::close(fd_);
        fd_ = -1;
        connectNext();
        return;
    }
    // 自连接检查：本地地址与对端相同（连本机空闲端口时可能出现）
    struct sockaddr_storage local;
    socklen_t local_len = sizeof(local);
    if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&local), &local_len) == 0 &&
        local.ss_family == addrs_[next_ - 1].family()) {
        if (addrs_[next_ - 1].family() == AF_INET) {
            auto* a = reinterpret_cast<struct sockaddr_in*>(&local);
            auto* b = reinterpret_cast<struct sockaddr_in*>(addrs_[next_ - 1].sockaddr());
            if (a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port) {
                last_error_ = {ConnectErrorKind::refused, "self connection"};
                removeAndResetChannel();
                ::close(fd_);
                fd_ = -1;
                connectNext();
                return;
            }
        }
    }
    finishSuccess();
}

void Connector::finishSuccess() {
    if (finished_) return;
    finished_ = true;
    if (timeout_timer_.valid()) loop_->cancel(timeout_timer_);
    InetAddress peer = addrs_[next_ - 1];
    int fd = fd_;
    fd_ = -1;
    removeAndResetChannel();
    auto cb = std::move(on_connected_);
    if (cb) cb(fd, peer);
}

void Connector::finishFailure(ConnectErrorKind kind, const std::string& message) {
    SPDLOG_DEBUG("connector: finishFailure kind={} message={}", static_cast<int>(kind), message);
    if (finished_) return;
    finished_ = true;
    if (timeout_timer_.valid()) loop_->cancel(timeout_timer_);
    removeAndResetChannel();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    auto cb = std::move(on_failed_);
    if (cb) cb(ConnectError{kind, message});
}

void Connector::cancel() {
    if (finished_) return;
    cancelled_ = true;
    finished_ = true;
    if (timeout_timer_.valid()) loop_->cancel(timeout_timer_);
    removeAndResetChannel();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    on_connected_ = nullptr;
    on_failed_ = nullptr;
}

}  // namespace gw::net
