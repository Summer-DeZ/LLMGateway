#pragma once

#include <atomic>
#include <map>
#include <nlohmann/json.hpp>

#include "gateway/context.hpp"
#include "gateway/key_store.hpp"
#include "http/http_server.hpp"

namespace gw::gateway {

std::string managementName(const nlohmann::json& body);
std::map<std::string, std::string> managementQuery(std::string_view query);
bool managementSourceAllowed(const http::HttpRequest& request, std::string_view peer, std::uint16_t port);
bool managementId(std::string_view id);
void managementDates(std::string_view start, std::string_view end);

class ManagementApi {
   public:
    ManagementApi(GatewayContext& ctx, http::HttpServer& server, net::EventLoop* ledger_loop, KeyStore& keys);
    void handle(const http::HttpRequest& request, const net::TcpConnectionPtr& conn);
    std::atomic<int> pending{0};
    void stopCompletions();  // Ledger-loop barrier before IO loops are destroyed.
   private:
    using Json = nlohmann::json;
    void reply(const net::TcpConnectionPtr& conn, int status, const Json& body);
    void query(const net::TcpConnectionPtr& conn, std::string stmt, store::PgParams params, bool mutation,
               std::function<void(const net::TcpConnectionPtr&, const Json&)> done);
    GatewayContext& ctx_;
    http::HttpServer& server_;
    net::EventLoop* ledger_loop_;
    KeyStore& keys_;
    std::uint16_t port_;
    bool stopping_ = false;  // ledger loop only
};
}  // namespace gw::gateway
