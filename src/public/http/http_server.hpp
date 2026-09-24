#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "base/noncopyable.hpp"
#include "http/http_request.hpp"
#include "http/request_parser.hpp"
#include "net/buffer.hpp"
#include "net/event_loop.hpp"
#include "net/event_loop_thread.hpp"
#include "net/inet_address.hpp"
#include "net/tcp_connection.hpp"
#include "net/tcp_server.hpp"

namespace gw::http {

struct HttpServerOptions {
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::size_t high_water_bytes = 256 * 1024;
    std::chrono::milliseconds header_timeout{30000};
    std::chrono::milliseconds body_timeout{60000};
    std::chrono::milliseconds keepalive_idle{75000};
    // 下游连接的 SO_SNDBUF；0 表示不设置。不限制时内核发送缓冲会自动增长到数 MB，用户态高水位很难触发
    int downstream_sndbuf_bytes = 64 * 1024;
};

// 挂在下游 TcpConnection::context() 上
struct HttpConnState {
    explicit HttpConnState(ParserLimits limits) : parser(limits) {}

    RequestParser parser;
    net::Buffer pending;          // 未解析完/被推迟解析的输入
    std::shared_ptr<void> session;  // 进行中的 ProxySession（gateway 侧填充）
    net::TimerId header_timer;
    net::TimerId body_timer;
    net::TimerId keepalive_timer;
    bool busy = false;
    bool draining = false;
    bool got_request_line = false;
};

class HttpServer : noncopyable {
public:
    using RequestHandler = std::function<void(const HttpRequest&, const net::TcpConnectionPtr&)>;
    using CloseHandler = std::function<void(const net::TcpConnectionPtr&)>;
    using HighWaterHandler = std::function<void(const net::TcpConnectionPtr&, std::size_t)>;
    using WriteCompleteHandler = std::function<void(const net::TcpConnectionPtr&)>;
    using ThreadInitCallback = net::EventLoopThread::ThreadInitCallback;

    HttpServer(net::EventLoop* loop, const net::InetAddress& listen_addr, std::string name, HttpServerOptions options);
    ~HttpServer();

    void setThreadNum(int num_threads) { tcp_server_->setThreadNum(num_threads); }
    void setThreadInitCallback(const ThreadInitCallback& cb) { tcp_server_->setThreadInitCallback(cb); }
    void setRequestHandler(RequestHandler handler) { request_handler_ = std::move(handler); }
    void setCloseHandler(CloseHandler handler) { close_handler_ = std::move(handler); }
    void setHighWaterHandler(HighWaterHandler handler) { high_water_handler_ = std::move(handler); }
    void setWriteCompleteHandler(WriteCompleteHandler handler) { write_complete_handler_ = std::move(handler); }

    void start();
    void stopAccepting();
    // 优雅退出：新请求 503 + close；空闲连接直接关闭
    void drain();
    // 在 base loop 线程上调用：销毁剩余连接并停止 io 线程。之后各 handler 不再被调用，
    // 所以必须在 handler 引用的对象（Router 等）析构之前调用
    void stop();
    // fn 在每个连接所属的 io loop 上执行；只能在 base loop 线程上调用
    void forEachConnection(const std::function<void(const net::TcpConnectionPtr&)>& fn);

    static HttpConnState* stateOf(const net::TcpConnectionPtr& conn);
    // ProxySession 结束后的回调：解除 busy，继续解析缓冲里的下一个请求
    void finishRequest(const net::TcpConnectionPtr& conn);
    // 给错误响应写 100-continue
    void sendContinue(const net::TcpConnectionPtr& conn);

    net::TcpServer* tcpServer() { return tcp_server_.get(); }
    const std::string& listenAddress() const { return tcp_server_->listenAddress(); }

private:
    void onConnection(const net::TcpConnectionPtr& conn);
    void onMessage(const net::TcpConnectionPtr& conn, net::Buffer* buf, gw::Timestamp receive_time);
    void processBuffer(const net::TcpConnectionPtr& conn, HttpConnState* state);
    void handleParseError(const net::TcpConnectionPtr& conn, HttpConnState* state);
    void startHeaderTimer(const net::TcpConnectionPtr& conn, HttpConnState* state);
    void startBodyTimer(const net::TcpConnectionPtr& conn, HttpConnState* state);
    void startKeepaliveTimer(const net::TcpConnectionPtr& conn, HttpConnState* state);
    void cancelTimers(const net::TcpConnectionPtr& conn, HttpConnState* state);

    net::EventLoop* loop_;
    HttpServerOptions options_;
    std::unique_ptr<net::TcpServer> tcp_server_;
    RequestHandler request_handler_;
    CloseHandler close_handler_;
    HighWaterHandler high_water_handler_;
    WriteCompleteHandler write_complete_handler_;
};

}  // namespace gw::http
