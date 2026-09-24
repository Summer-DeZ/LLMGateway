#include "net/tcp_connection.hpp"

#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cassert>
#include <cstring>

#include "base/log.hpp"
#include "net/event_loop.hpp"
#include "net/socket.hpp"

namespace gw::net {

TcpConnection::TcpConnection(EventLoop* loop, std::string name, int sockfd, InetAddress local_addr,
                             InetAddress peer_addr, std::unique_ptr<Transport> transport)
    : loop_(loop),
      name_(std::move(name)),
      state_(State::kConnecting),
      reading_(true),
      transport_(std::move(transport)),
      channel_(loop, sockfd),
      high_water_mark_(64 * 1024 * 1024),
      local_addr_(std::move(local_addr)),
      peer_addr_(std::move(peer_addr)) {
    channel_.setReadCallback([this] { handleRead(loop_->pollReturnTime()); });
    channel_.setWriteCallback([this] { handleWrite(); });
    channel_.setCloseCallback([this] { handleClose(); });
    channel_.setErrorCallback([this] { handleError(); });
}

TcpConnection::~TcpConnection() {
    assert(state_ == State::kDisconnected);
}

void TcpConnection::setState(State state) {
    state_ = state;
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    assert(state_ == State::kConnecting);
    setState(State::kConnected);
    channel_.tie(shared_from_this());
    channel_.enableReading();
    if (connection_callback_) connection_callback_(shared_from_this());
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ == State::kConnected || state_ == State::kDisconnecting) {
        bool was_connected = state_ == State::kConnected;
        setState(State::kDisconnected);
        channel_.disableAll();
        if (was_connected && connection_callback_) connection_callback_(shared_from_this());
    }
    channel_.remove();
}

void TcpConnection::send(std::string_view data) {
    if (state_ == State::kDisconnected) return;
    if (loop_->isInLoopThread()) {
        sendInLoop(data);
    } else {
        std::string copy(data);
        loop_->runInLoop([this, copy = std::move(copy)] {
            if (state_ != State::kDisconnected) sendInLoop(copy);
        });
    }
}

void TcpConnection::send(Buffer* buf) {
    if (state_ == State::kDisconnected) return;
    if (loop_->isInLoopThread()) {
        sendBufferInLoop(buf);
    } else {
        Buffer copy;
        copy.append(buf->peek(), buf->readableBytes());
        buf->retrieveAll();
        loop_->runInLoop([this, buf = std::move(copy)]() mutable {
            if (state_ != State::kDisconnected) sendBufferInLoop(&buf);
        });
    }
}

void TcpConnection::sendBufferInLoop(Buffer* buf) {
    std::size_t remaining = buf->readableBytes();
    if (remaining == 0) return;
    sendInLoop(std::string_view(buf->peek(), remaining));
    buf->retrieveAll();
}

void TcpConnection::sendInLoop(std::string_view data) {
    loop_->assertInLoopThread();
    if (state_ == State::kDisconnected || write_fault_) return;

    std::size_t remaining = data.size();
    if (remaining == 0) return;

    const std::size_t old_len = output_buffer_.readableBytes();
    bool fault = false;

    if (old_len == 0 && !channel_.isWriting()) {
        IoResult r = transport_->write(data.data(), remaining);
        if (r.status == IoStatus::error) {
            last_error_ = transport_->lastError();
            fault = true;
        } else if (r.status == IoStatus::ok) {
            remaining -= r.bytes;
            data.remove_prefix(r.bytes);
        }
    }

    if (!fault && remaining > 0) {
        output_buffer_.append(data.data(), remaining);
        if (!channel_.isWriting()) {
            channel_.enableWriting();
        }
    }

    if (!fault && high_water_mark_callback_ != nullptr && old_len < high_water_mark_ &&
        output_buffer_.readableBytes() >= high_water_mark_) {
        TcpConnectionPtr conn(shared_from_this());
        std::size_t mark = output_buffer_.readableBytes();
        loop_->queueInLoop([conn, mark] {
            if (conn->high_water_mark_callback_) conn->high_water_mark_callback_(conn, mark);
        });
    }

    if (fault) {
        // 不在调用方栈上同步关闭：调用方可能正处在上游数据的解析回调里（ProxySession 转发 SSE），
        // 同步 handleClose 会经 close 回调重入会话的取消路径。之后的 send 直接丢弃。
        write_fault_ = true;
        handleError();
        TcpConnectionPtr conn(shared_from_this());
        loop_->queueInLoop([conn] { conn->handleClose(); });
    }
}

void TcpConnection::handleRead(Timestamp receive_time) {
    loop_->assertInLoopThread();
    // 已关闭但对象还活着（handleClose 把析构推迟到本轮之后）时，本轮剩下的事件直接忽略
    if (state_ == State::kDisconnected) return;
    IoResult r = transport_->readInto(input_buffer_);
    if (r.bytes > 0) {
        if (message_callback_) message_callback_(shared_from_this(), &input_buffer_, receive_time);
    }
    if (r.status == IoStatus::eof) {
        handleClose();
        return;
    }
    if (r.status == IoStatus::error) {
        last_error_ = transport_->lastError();
        handleError();
        handleClose();
    }
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (!channel_.isWriting()) return;
    if (output_buffer_.readableBytes() == 0) {
        channel_.disableWriting();
        return;
    }
    IoResult r = transport_->write(output_buffer_.peek(), output_buffer_.readableBytes());
    if (r.status == IoStatus::error) {
        last_error_ = transport_->lastError();
        handleError();
        forceCloseInLoop();
        return;
    }
    if (r.bytes > 0) output_buffer_.retrieve(r.bytes);

    if (output_buffer_.readableBytes() == 0) {
        channel_.disableWriting();
        if (write_complete_callback_) {
            TcpConnectionPtr conn(shared_from_this());
            loop_->queueInLoop([conn] {
                if (conn->state_ != State::kDisconnected && conn->write_complete_callback_)
                    conn->write_complete_callback_(conn);
            });
        }
        if (state_ == State::kDisconnecting) {
            shutdownInLoop();
        }
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    if (state_ == State::kDisconnected) return;
    setState(State::kDisconnected);
    channel_.disableAll();
    TcpConnectionPtr guard(shared_from_this());
    if (connection_callback_) connection_callback_(guard);
    if (close_callback_) close_callback_(guard);
    channel_.remove();
    // EventLoop 正在遍历本轮 epoll 返回的裸 Channel* 列表。上游连接的最后一个引用可能随会话在上面的回调里释放，
    // 若本连接的 Channel 还排在本轮后面就会被访问已释放内存。把析构推迟到本轮事件处理完之后。
    loop_->queueInLoop([guard] {});
}

void TcpConnection::handleError() {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(channel_.fd(), SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err != 0) {
        last_error_ = std::strerror(err);
    }
}

void TcpConnection::shutdown() {
    if (state_ == State::kDisconnected) return;
    if (loop_->isInLoopThread()) {
        shutdownInLoop();
    } else {
        loop_->runInLoop([this] {
            if (state_ != State::kDisconnected) shutdownInLoop();
        });
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (state_ == State::kDisconnected) return;
    if (!channel_.isWriting()) {
        setState(State::kDisconnecting);
        transport_->shutdownWrite();
    } else {
        setState(State::kDisconnecting);
    }
}

void TcpConnection::forceClose() {
    if (state_ == State::kDisconnected) return;
    if (loop_->isInLoopThread()) {
        forceCloseInLoop();
    } else {
        TcpConnectionPtr conn(shared_from_this());
        loop_->runInLoop([conn] {
            if (conn->state_ != State::kDisconnected) conn->forceCloseInLoop();
        });
    }
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    if (state_ == State::kDisconnected) return;
    handleClose();
}

void TcpConnection::abortAndDrain(const std::function<void(std::string_view)>& on_data, std::size_t limit,
                                  const std::function<void()>& on_shutdown) {
    loop_->assertInLoopThread();
    if (state_ == State::kDisconnected) return;
    transport_->shutdownWrite();
    if (on_shutdown) on_shutdown();
    std::size_t total = 0;
    while (total < limit) {
        Buffer tmp;
        IoResult r = transport_->readInto(tmp);
        if (r.bytes > 0) {
            if (on_data) on_data(std::string_view(tmp.peek(), tmp.readableBytes()));
            total += r.bytes;
        }
        if (r.status == IoStatus::would_block || r.status == IoStatus::eof || r.status == IoStatus::error) break;
        if (r.bytes == 0) break;
    }
    forceCloseInLoop();
}

void TcpConnection::startRead() {
    TcpConnectionPtr conn(shared_from_this());
    loop_->runInLoop([conn] {
        if (conn->state_ == State::kDisconnected || conn->reading_) return;
        conn->reading_ = true;
        conn->channel_.enableReading();
    });
}

void TcpConnection::stopRead() {
    TcpConnectionPtr conn(shared_from_this());
    loop_->runInLoop([conn] {
        if (conn->state_ == State::kDisconnected || !conn->reading_) return;
        conn->reading_ = false;
        conn->channel_.disableReading();
    });
}

void TcpConnection::setTcpNoDelay(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(channel_.fd(), IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

void TcpConnection::setSendBufferSize(int bytes) {
    ::setsockopt(channel_.fd(), SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

}  // namespace gw::net
