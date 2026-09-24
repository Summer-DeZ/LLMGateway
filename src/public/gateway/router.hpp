#pragma once

#include <memory>
#include <string>

#include "base/noncopyable.hpp"
#include "gateway/context.hpp"
#include "http/http_request.hpp"
#include "http/http_server.hpp"
#include "net/tcp_connection.hpp"
#include "proto/neutral.hpp"

namespace gw::gateway {

class Router : noncopyable {
public:
    Router(GatewayContext& context, http::HttpServer& server);
    ~Router();

    void handle(const http::HttpRequest& request, const net::TcpConnectionPtr& conn);
    // 下游连接关闭：取消进行中的会话
    void onConnectionClosed(const net::TcpConnectionPtr& conn);
    void onHighWater(const net::TcpConnectionPtr& conn, std::size_t mark);
    void onWriteComplete(const net::TcpConnectionPtr& conn);

    std::size_t activeSessions() const { return context_.active_sessions.load(); }

private:
    void handleProxy(proto::Protocol protocol, const http::HttpRequest& request,
                     const net::TcpConnectionPtr& conn, const UserConfig& user);
    void handleModels(const http::HttpRequest& request, const net::TcpConnectionPtr& conn);
    void methodNotAllowed(const http::HttpRequest& request, const net::TcpConnectionPtr& conn,
                          std::string_view allow);

    GatewayContext& context_;
    http::HttpServer& server_;
};

}  // namespace gw::gateway
