#pragma once

#include <memory>
#include <optional>
#include <string>

#include "base/noncopyable.hpp"
#include "gateway/config.hpp"
#include "gateway/context.hpp"
#include "gateway/quota.hpp"
#include "http/http_request.hpp"
#include "net/event_loop.hpp"
#include "net/tcp_connection.hpp"
#include "gateway/pricing.hpp"
#include "store/pg_client.hpp"

namespace gw::http {
class HttpServer;
}

namespace gw::gateway {

// /v1/usage、/v1/usage/requests、/admin/usage*、/admin/quota/reconcile（usage-ledger.md §7）。
// PG 查询投递到 ledger loop 的读连接执行，结果 queueInLoop 回发起请求的 io loop 再写出；
// 配额部分从调用方 loop 的 RedisClient 实时读取。io 线程不做任何阻塞 I/O。
// 所有响应异步组装（闭包携带中间状态），UsageApi 本身无会话状态、不可变共享。
class UsageApi : noncopyable {
public:
    UsageApi(GatewayContext& context, http::HttpServer& server, net::EventLoop* ledger_loop,
             std::shared_ptr<store::PgClient> read_client, QuotaManager* quota);

    void handleUsage(const http::HttpRequest& request, const net::TcpConnectionPtr& conn, const UserConfig& user);
    void handleUsageRequests(const http::HttpRequest& request, const net::TcpConnectionPtr& conn,
                             const UserConfig& user);
    void handleAdminUsage(const http::HttpRequest& request, const net::TcpConnectionPtr& conn);
    void handleAdminUsageRequests(const http::HttpRequest& request, const net::TcpConnectionPtr& conn);
    void handleReconcile(const http::HttpRequest& request, const net::TcpConnectionPtr& conn,
                         std::optional<std::string> key);

private:
    void writeJson(const net::TcpConnectionPtr& conn, int status, const std::string& body);
    // 取调用方 loop 的 Redis 连接（未配置 → nullptr）
    store::RedisClient* redisOf(const net::TcpConnectionPtr& conn) const;
    // PG 查询：caller loop 发起，ledger loop 的读连接执行，结果搬回 caller loop
    void pgQuery(const net::TcpConnectionPtr& conn, const std::string& stmt, store::PgParams params,
                 std::function<void(std::expected<store::PgResult, store::PgError>)> on_done);

    GatewayContext& context_;
    http::HttpServer& server_;
    net::EventLoop* ledger_loop_;
    std::shared_ptr<store::PgClient> read_client_;
    QuotaManager* quota_;
};

}  // namespace gw::gateway
