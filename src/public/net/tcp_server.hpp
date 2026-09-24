#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "base/noncopyable.hpp"
#include "net/acceptor.hpp"
#include "net/callbacks.hpp"
#include "net/event_loop_thread.hpp"
#include "net/event_loop_thread_pool.hpp"
#include "net/inet_address.hpp"
#include "net/tcp_connection.hpp"

namespace gw::net {

class EventLoop;

class TcpServer : noncopyable {
public:
    using ThreadInitCallback = EventLoopThread::ThreadInitCallback;

    TcpServer(EventLoop* loop, const InetAddress& listen_addr, std::string name, bool reuse_port = true);
    ~TcpServer();

    void setThreadNum(int num_threads) { thread_pool_->setThreadNum(num_threads); }
    void setThreadInitCallback(const ThreadInitCallback& cb) { thread_init_callback_ = cb; }

    void start();
    void stopAccepting();

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    const std::string& listenAddress() const { return listen_address_; }

    void setConnectionCallback(const ConnectionCallback& cb) { connection_callback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { message_callback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { write_complete_callback_ = cb; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback& cb, std::size_t mark) {
        high_water_mark_callback_ = cb;
        high_water_mark_ = mark;
    }

    // 遍历所有连接（在各自 io loop 上执行 fn），用于优雅退出
    void forEachConnection(const std::function<void(const TcpConnectionPtr&)>& fn);

private:
    void newConnection(int sockfd, const InetAddress& peer_addr);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    const std::string name_;
    const std::string listen_address_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<EventLoopThreadPool> thread_pool_;
    ThreadInitCallback thread_init_callback_;
    bool started_;

    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    WriteCompleteCallback write_complete_callback_;
    HighWaterMarkCallback high_water_mark_callback_;
    std::size_t high_water_mark_ = 64 * 1024 * 1024;

    std::map<std::string, TcpConnectionPtr> connections_;
    std::atomic<std::uint64_t> next_conn_id_{1};
};

}  // namespace gw::net
