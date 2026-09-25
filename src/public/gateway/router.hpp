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

class UsageApi;

class Router : noncopyable {
public:
    Router(GatewayContext& context, http::HttpServer& server, UsageApi* usage_api = nullptr);
    ~Router();

    void handle(const http::HttpRequest& request, const net::TcpConnectionPtr& conn);
    // 下游连接关闭：取消进行中的会话
    void onConnectionClosed(const net::TcpConnectionPtr& conn);
    // 退出超时：取消所有进行中的会话（与客户端断开同一路径）。只能在 base loop 线程上调用，取消在各 io loop 上异步执行
    void cancelAll(const std::string& reason);

private:
    void handleProxy(proto::Protocol protocol, const http::HttpRequest& request,
                     const net::TcpConnectionPtr& conn, std::shared_ptr<const UserConfig> user);
    void handleModels(const http::HttpRequest& request, const net::TcpConnectionPtr& conn);
    void methodNotAllowed(const http::HttpRequest& request, const net::TcpConnectionPtr& conn,
                          std::string_view allow);

    GatewayContext& context_;
    http::HttpServer& server_;
    UsageApi* usage_api_ = nullptr;
};

}  // namespace gw::gateway
