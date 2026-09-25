#include "gateway/router.hpp"

#include <cctype>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "base/log.hpp"
#include "base/random.hpp"
#include "gateway/access_log.hpp"
#include "gateway/admin.hpp"
#include "gateway/auth.hpp"
#include "gateway/metrics.hpp"
#include "gateway/proxy_session.hpp"
#include "gateway/request_record.hpp"
#include "gateway/usage_api.hpp"
#include "http/response_writer.hpp"
#include "proto/errors.hpp"

namespace gw::gateway {

namespace {

proto::Protocol guessProtocol(const std::string& path) {
    if (path.starts_with("/v1/messages")) return proto::Protocol::anthropic;
    return proto::Protocol::openai;
}

std::shared_ptr<ProxySession> sessionOf(const net::TcpConnectionPtr& conn) {
    auto* state = http::HttpServer::stateOf(conn);
    if (state == nullptr || !state->session) return nullptr;
    return std::static_pointer_cast<ProxySession>(state->session);
}

std::optional<std::string> queryParam(const std::string& query, std::string_view name) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        std::string_view item = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        std::size_t eq = item.find('=');
        if (eq != std::string_view::npos && item.substr(0, eq) == name) {
            return std::string(item.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::nullopt;
}

// 无会话请求（401 等）的访问日志一行
void logRejectedRequest(GatewayContext& context, const http::HttpRequest& request, const char* outcome, int status,
                        const UserConfig* user) {
    if (!context.access_log && !context.recent) return;
    auto record = std::make_shared<RequestRecord>();
    record->ts = gw::wallMillis();
    record->request_id = request.header("x-request-id") ? std::string(*request.header("x-request-id"))
                                                        : gw::randomHex(16);
    record->user = user ? user->name : "-";
    record->ingress = guessProtocol(request.path) == proto::Protocol::anthropic ? "anthropic" : "openai";
    record->model = "-";
    record->outcome = outcome;
    record->http_status = status;
    record->cache = "none";
    record->usage_source = "rejected";
    if (context.metrics) context.metrics->observeRequest(*record);
    if (context.access_log) context.access_log->write(*record);
    if (context.recent) context.recent->push(record);
}

}  // namespace

Router::Router(GatewayContext& context, http::HttpServer& server, UsageApi* usage_api)
    : context_(context), server_(server), usage_api_(usage_api) {
    server_.setHighWaterHandler([this](const net::TcpConnectionPtr& conn, std::size_t mark) {
        if (auto session = sessionOf(conn)) session->onDownstreamHighWater(mark);
    });
    server_.setWriteCompleteHandler([this](const net::TcpConnectionPtr& conn) {
        if (auto session = sessionOf(conn)) session->onDownstreamWriteComplete();
    });
}

Router::~Router() = default;

void Router::handle(const http::HttpRequest& request, const net::TcpConnectionPtr& conn) {
    const std::string& path = request.path;

    if (path == "/healthz") {
        if (request.method != "GET") {
            methodNotAllowed(request, conn, "GET");
            return;
        }
        http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
        writer.writeFull(200, {}, "text/plain", "ok");
        server_.finishRequest(conn);
        return;
    }

    if (path == "/metrics" || path == "/admin/stats" || path == "/admin/requests" ||
        path == "/admin/usage" || path == "/admin/usage/requests" || path == "/admin/quota/reconcile") {
        if (request.method != "GET") {
            methodNotAllowed(request, conn, "GET");
            return;
        }
        if (!peerAllowed(conn, context_.config->admin.listen_local_only)) {
            http::ResponseWriter writer(conn, gw::randomHex(16), false);
            writer.writeFull(403, {}, "text/plain", "forbidden");
            server_.finishRequest(conn);
            return;
        }
        if (path == "/admin/usage" || path == "/admin/usage/requests" || path == "/admin/quota/reconcile") {
            if (usage_api_ != nullptr) {
                if (path == "/admin/usage") usage_api_->handleAdminUsage(request, conn);
                else if (path == "/admin/usage/requests") usage_api_->handleAdminUsageRequests(request, conn);
                else usage_api_->handleReconcile(request, conn, queryParam(request.query, "key"));
                return;  // 异步回复（UsageApi 接管连接）
            }
            http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
            writer.writeFull(503, {}, "application/json",
                             proto::errorBody(guessProtocol(path), "usage api not configured", "service_unavailable"));
            server_.finishRequest(conn);
            return;
        }
        http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
        // 可观测性接口的缺陷不允许拖垮代理进程
        try {
            if (path == "/metrics") {
                std::string body = context_.metrics ? context_.metrics->renderPrometheus() : "";
                writer.writeFull(200, {}, "text/plain; version=0.0.4; charset=utf-8", body);
            } else if (path == "/admin/stats") {
                std::string body = context_.metrics ? context_.metrics->renderStats() : "{}";
                writer.writeFull(200, {}, "application/json", body);
            } else {
                std::size_t limit = 20;
                if (auto l = queryParam(request.query, "limit")) {
                    unsigned long v = std::strtoul(l->c_str(), nullptr, 10);
                    if (v > 0 && v <= 256) limit = static_cast<std::size_t>(v);
                }
                std::string body = context_.recent ? context_.recent->renderJson(limit) : "[]";
                writer.writeFull(200, {}, "application/json", body);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("admin endpoint {} failed: {}", path, e.what());
            writer.writeFull(500, {}, "text/plain", "internal error");
        }
        server_.finishRequest(conn);
        return;
    }

    if (path == "/v1/usage" || path == "/v1/usage/requests") {
        if (request.method != "GET") {
            methodNotAllowed(request, conn, "GET");
            return;
        }
        auto auth = authenticate(*context_.config, request);
        if (auth.user == nullptr) {
            http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
            writer.writeFull(401, {}, "application/json",
                             proto::errorBody(proto::Protocol::openai, "invalid api key", "authentication_error"));
            logRejectedRequest(context_, request, "unauthorized", 401, nullptr);
            server_.finishRequest(conn);
            return;
        }
        if (usage_api_ != nullptr) {
            if (path == "/v1/usage") usage_api_->handleUsage(request, conn, *auth.user);
            else usage_api_->handleUsageRequests(request, conn, *auth.user);
            return;  // 异步回复
        }
        http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
        writer.writeFull(503, {}, "application/json",
                         proto::errorBody(proto::Protocol::openai, "usage api not configured",
                                          "service_unavailable"));
        server_.finishRequest(conn);
        return;
    }

    if (path == "/v1/models") {
        if (request.method != "GET") {
            methodNotAllowed(request, conn, "GET");
            return;
        }
        handleModels(request, conn);
        return;
    }

    // /v1/messages（Anthropic 入口）在 G14 实现之前按未知路径处理
    if (path == "/v1/chat/completions") {
        if (request.method != "POST") {
            methodNotAllowed(request, conn, "POST");
            return;
        }
        proto::Protocol protocol = proto::Protocol::openai;
        auto auth = authenticate(*context_.config, request);
        if (auth.user == nullptr) {
            http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
            writer.writeFull(401, {}, "application/json",
                             proto::errorBody(protocol, "invalid api key", "authentication_error"));
            logRejectedRequest(context_, request, "unauthorized", 401, nullptr);
            server_.finishRequest(conn);
            return;
        }
        handleProxy(protocol, request, conn, *auth.user);
        return;
    }

    http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
    writer.writeFull(404, {}, "application/json",
                     proto::errorBody(guessProtocol(path), "unknown path: " + path, "invalid_request_error"));
    server_.finishRequest(conn);
}

void Router::methodNotAllowed(const http::HttpRequest& request, const net::TcpConnectionPtr& conn,
                              std::string_view allow) {
    http::Headers headers{{"allow", std::string(allow)}};
    http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
    writer.writeFull(405, headers, "application/json",
                     proto::errorBody(guessProtocol(request.path), "method not allowed", "invalid_request_error"));
    server_.finishRequest(conn);
}

void Router::handleModels(const http::HttpRequest& request, const net::TcpConnectionPtr& conn) {
    auto auth = authenticate(*context_.config, request);
    if (auth.user == nullptr) {
        http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
        writer.writeFull(401, {}, "application/json",
                         proto::errorBody(proto::Protocol::openai, "invalid api key", "authentication_error"));
        logRejectedRequest(context_, request, "unauthorized", 401, nullptr);
        server_.finishRequest(conn);
        return;
    }
    nlohmann::json out;
    out["object"] = "list";
    out["data"] = nlohmann::json::array();
    for (const auto& alias : context_.config->model_order) {
        nlohmann::json model;
        model["id"] = alias;
        model["object"] = "model";
        model["created"] = 0;
        model["owned_by"] = "llm-gateway";
        out["data"].push_back(std::move(model));
    }
    http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
    writer.writeFull(200, {}, "application/json", out.dump());
    server_.finishRequest(conn);
}

void Router::handleProxy(proto::Protocol protocol, const http::HttpRequest& request,
                         const net::TcpConnectionPtr& conn, const UserConfig& user) {
    auto* state = http::HttpServer::stateOf(conn);
    if (state == nullptr) return;
    if (state->session) {
        SPDLOG_WARN("router: connection {} already has an active session", conn->name());
        return;
    }
    ProxySession::Ingress ingress;
    ingress.protocol = protocol;
    if (auto id = request.header("x-request-id"); id && !id->empty()) {
        ingress.request_id = std::string(*id);
    } else {
        ingress.request_id = gw::randomHex(16);
    }
    ingress.received_at = nowSteady();
    ingress.http = request;
    ingress.user = &user;

    auto session = std::make_shared<ProxySession>(context_, server_, conn, std::move(ingress));
    state->session = std::static_pointer_cast<void>(session);
    session->start();
}

void Router::onConnectionClosed(const net::TcpConnectionPtr& conn) {
    if (auto session = sessionOf(conn)) {
        session->cancel("downstream closed");
    }
}

void Router::cancelAll(const std::string& reason) {
    server_.forEachConnection([reason](const net::TcpConnectionPtr& conn) {
        if (auto session = sessionOf(conn)) session->cancel(reason);
    });
}

}  // namespace gw::gateway
