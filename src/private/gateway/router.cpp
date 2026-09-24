#include "gateway/router.hpp"

#include <nlohmann/json.hpp>

#include "base/log.hpp"
#include "base/random.hpp"
#include "gateway/auth.hpp"
#include "gateway/proxy_session.hpp"
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

}  // namespace

Router::Router(GatewayContext& context, http::HttpServer& server) : context_(context), server_(server) {
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

    if (path == "/v1/models") {
        if (request.method != "GET") {
            methodNotAllowed(request, conn, "GET");
            return;
        }
        handleModels(request, conn);
        return;
    }

    if (path == "/v1/chat/completions" || path == "/v1/messages") {
        if (request.method != "POST") {
            methodNotAllowed(request, conn, "POST");
            return;
        }
        proto::Protocol protocol = path == "/v1/messages" ? proto::Protocol::anthropic : proto::Protocol::openai;
        auto auth = authenticate(*context_.config, request);
        if (auth.user == nullptr) {
            http::ResponseWriter writer(conn, gw::randomHex(16), request.keepAlive());
            writer.writeFull(401, {}, "application/json",
                             proto::errorBody(protocol, "invalid api key", "authentication_error"));
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

void Router::onHighWater(const net::TcpConnectionPtr& conn, std::size_t mark) {
    if (auto session = sessionOf(conn)) session->onDownstreamHighWater(mark);
}

void Router::onWriteComplete(const net::TcpConnectionPtr& conn) {
    if (auto session = sessionOf(conn)) session->onDownstreamWriteComplete();
}

}  // namespace gw::gateway
