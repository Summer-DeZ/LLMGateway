#pragma once

#include <any>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "net/buffer.hpp"
#include "net/callbacks.hpp"
#include "net/channel.hpp"
#include "net/inet_address.hpp"
#include "net/transport.hpp"

namespace gw::net {

class EventLoop;

class TcpConnection : noncopyable, public std::enable_shared_from_this<TcpConnection> {
public:
    enum class State { kConnecting, kConnected, kDisconnecting, kDisconnected };

    TcpConnection(EventLoop* loop, std::string name, int sockfd, InetAddress local_addr, InetAddress peer_addr,
                  std::unique_ptr<Transport> transport);
    ~TcpConnection();

    // 跨线程安全（非本线程时复制后 runInLoop）
    void send(std::string_view data);
    void send(Buffer* buf);

    void shutdown();     // 输出缓冲写完后再半关闭
    void forceClose();   // 立即关闭，丢弃输出缓冲

    // 立即 shutdownWrite → 非阻塞读完已到达的字节交给 onData → close。仅 loop 线程调用。
    // on_shutdown 在 shutdownWrite 之后、排空之前调用（用于测量取消延迟）
    void abortAndDrain(const std::function<void(std::string_view)>& on_data, std::size_t limit = 1 << 20,
                       const std::function<void()>& on_shutdown = {});

    void startRead();
    void stopRead();
    bool isReading() const { return reading_; }

    void setTcpNoDelay(bool on);
    void setSendBufferSize(int bytes);

    std::size_t outputBytes() const { return output_buffer_.readableBytes(); }

    void setConnectionCallback(ConnectionCallback cb) { connection_callback_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb) { message_callback_ = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { write_complete_callback_ = std::move(cb); }
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t mark) {
        high_water_mark_callback_ = std::move(cb);
        high_water_mark_ = mark;
    }
    void setCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }

    const std::string& name() const { return name_; }
    EventLoop* getLoop() const { return loop_; }
    State state() const { return state_; }
    bool connected() const { return state_ == State::kConnected; }
    bool disconnected() const { return state_ == State::kDisconnected; }
    const InetAddress& localAddress() const { return local_addr_; }
    const InetAddress& peerAddress() const { return peer_addr_; }
    const std::string& lastError() const { return last_error_; }

    std::any& context() { return context_; }

    void connectEstablished();
    void connectDestroyed();

private:
    void handleRead(Timestamp receive_time);
    void handleWrite();
    void handleClose();
    void handleError();
    void sendInLoop(std::string_view data);
    void sendBufferInLoop(Buffer* buf);
    void shutdownInLoop();
    void forceCloseInLoop();
    void setState(State state);

    EventLoop* loop_;
    const std::string name_;
    State state_;
    bool reading_;
    std::unique_ptr<Transport> transport_;
    Channel channel_;
    Buffer input_buffer_;
    Buffer output_buffer_;
    std::size_t high_water_mark_;
    InetAddress local_addr_;
    InetAddress peer_addr_;
    std::string last_error_;
    bool write_fault_ = false;
    std::any context_;

    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    WriteCompleteCallback write_complete_callback_;
    HighWaterMarkCallback high_water_mark_callback_;
    CloseCallback close_callback_;
};

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

}  // namespace gw::net
