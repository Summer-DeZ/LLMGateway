#include "http/http_server.hpp"

#include <any>

#include "base/log.hpp"
#include "http/response_writer.hpp"

namespace gw::http {

using namespace std::chrono_literals;

HttpServer::HttpServer(net::EventLoop* loop, const net::InetAddress& listen_addr, std::string name,
                       HttpServerOptions options)
    : loop_(loop), options_(options) {
    tcp_server_ = std::make_unique<net::TcpServer>(loop, listen_addr, std::move(name));
    tcp_server_->setConnectionCallback([this](const net::TcpConnectionPtr& conn) { onConnection(conn); });
    tcp_server_->setMessageCallback(
        [this](const net::TcpConnectionPtr& conn, net::Buffer* buf, gw::Timestamp t) { onMessage(conn, buf, t); });
    tcp_server_->setHighWaterMarkCallback(
        [this](const net::TcpConnectionPtr& conn, std::size_t mark) {
            if (high_water_handler_) high_water_handler_(conn, mark);
        },
        options_.high_water_bytes);
    tcp_server_->setWriteCompleteCallback([this](const net::TcpConnectionPtr& conn) {
        if (write_complete_handler_) write_complete_handler_(conn);
    });
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::stop() {
    loop_->assertInLoopThread();
    // TcpServer 析构时在各 io loop 上执行 connectDestroyed（会回调 onConnection），然后 join io 线程；
    // 先于本对象其余成员销毁，保证那时各 handler 仍然有效
    tcp_server_.reset();
}

void HttpServer::forEachConnection(const std::function<void(const net::TcpConnectionPtr&)>& fn) {
    if (tcp_server_) tcp_server_->forEachConnection(fn);
}

HttpConnState* HttpServer::stateOf(const net::TcpConnectionPtr& conn) {
    auto* state = std::any_cast<HttpConnState>(&conn->context());
    return state;
}

void HttpServer::start() {
    tcp_server_->start();
}

void HttpServer::stopAccepting() {
    if (tcp_server_) tcp_server_->stopAccepting();
}

void HttpServer::drain() {
    forEachConnection([](const net::TcpConnectionPtr& conn) {
        auto* state = stateOf(conn);
        if (!state) return;
        state->draining = true;
        if (!state->busy) {
            conn->shutdown();
        }
    });
}

void HttpServer::cancelTimers(const net::TcpConnectionPtr& conn, HttpConnState* state) {
    net::EventLoop* loop = conn->getLoop();
    if (state->header_timer.valid()) loop->cancel(state->header_timer);
    if (state->body_timer.valid()) loop->cancel(state->body_timer);
    if (state->keepalive_timer.valid()) loop->cancel(state->keepalive_timer);
    state->header_timer = net::TimerId{};
    state->body_timer = net::TimerId{};
    state->keepalive_timer = net::TimerId{};
}

void HttpServer::startHeaderTimer(const net::TcpConnectionPtr& conn, HttpConnState* state) {
    std::weak_ptr<net::TcpConnection> weak = conn;
    state->header_timer = conn->getLoop()->runAfter(options_.header_timeout, [this, weak] {
        auto c = weak.lock();
        if (!c) return;
        auto* st = stateOf(c);
        if (!st || st->busy) return;
        SPDLOG_DEBUG("downstream header timeout: {}", c->name());
        ResponseWriter writer(c, {}, false);
        writer.writeFull(408, {}, "text/plain", "request header timeout\n");
        c->shutdown();
    });
}

void HttpServer::startBodyTimer(const net::TcpConnectionPtr& conn, HttpConnState* state) {
    net::EventLoop* loop = conn->getLoop();
    if (state->body_timer.valid()) loop->cancel(state->body_timer);
    std::weak_ptr<net::TcpConnection> weak = conn;
    state->body_timer = loop->runAfter(options_.body_timeout, [this, weak] {
        auto c = weak.lock();
        if (!c) return;
        auto* st = stateOf(c);
        if (!st || st->busy || st->parser.state() != RequestParser::State::kBody) return;
        SPDLOG_DEBUG("downstream body timeout: {}", c->name());
        ResponseWriter writer(c, {}, false);
        writer.writeFull(408, {}, "text/plain", "request body timeout\n");
        c->shutdown();
    });
}

void HttpServer::startKeepaliveTimer(const net::TcpConnectionPtr& conn, HttpConnState* state) {
    net::EventLoop* loop = conn->getLoop();
    if (state->keepalive_timer.valid()) loop->cancel(state->keepalive_timer);
    std::weak_ptr<net::TcpConnection> weak = conn;
    state->keepalive_timer = loop->runAfter(options_.keepalive_idle, [this, weak] {
        auto c = weak.lock();
        if (!c) return;
        auto* st = stateOf(c);
        if (!st || st->busy || st->pending.readableBytes() > 0) return;
        SPDLOG_DEBUG("downstream keepalive idle timeout: {}", c->name());
        c->shutdown();
    });
}

void HttpServer::onConnection(const net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        if (options_.downstream_sndbuf_bytes > 0) conn->setSendBufferSize(options_.downstream_sndbuf_bytes);
        conn->context() = HttpConnState(ParserLimits{options_.max_body_bytes, options_.max_body_bytes});
        startHeaderTimer(conn, stateOf(conn));
    } else {
        auto* state = stateOf(conn);
        if (state) cancelTimers(conn, state);
        if (close_handler_) close_handler_(conn);
    }
}

void HttpServer::sendContinue(const net::TcpConnectionPtr& conn) {
    conn->send("HTTP/1.1 100 Continue\r\n\r\n");
}

void HttpServer::handleParseError(const net::TcpConnectionPtr& conn, HttpConnState* state) {
    int status = state->parser.errorStatus();
    cancelTimers(conn, state);
    ResponseWriter writer(conn, {}, false);
    std::string body = std::string(reasonPhrase(status)) + "\n";
    writer.writeFull(status, {}, "text/plain", body);
    conn->shutdown();
}

void HttpServer::processBuffer(const net::TcpConnectionPtr& conn, HttpConnState* state) {
    while (!state->busy) {
        RequestParser::Result result = state->parser.feed(state->pending);
        if (result == RequestParser::Result::need_more) {
            if (state->parser.headersComplete() && !state->body_timer.valid() &&
                state->parser.state() == RequestParser::State::kBody) {
                startBodyTimer(conn, state);
            }
            if (state->parser.expectContinue()) {
                sendContinue(conn);
                state->parser.setContinueSent();
            }
            return;
        }
        if (result == RequestParser::Result::error) {
            handleParseError(conn, state);
            return;
        }
        // complete
        cancelTimers(conn, state);
        HttpRequest request = state->parser.take();
        state->busy = true;
        if (state->draining) {
            ResponseWriter writer(conn, {}, false);
            writer.writeFull(503, {}, "application/json",
                             "{\"error\":{\"message\":\"server shutting down\",\"type\":\"service_unavailable\","
                             "\"code\":\"server_shutting_down\"}}");
            conn->shutdown();
            return;
        }
        if (request_handler_) {
            request_handler_(request, conn);
        } else {
            ResponseWriter writer(conn, {}, request.keepAlive());
            writer.writeFull(404, {}, "text/plain", "not found\n");
            finishRequest(conn);
        }
        return;
    }
}

void HttpServer::onMessage(const net::TcpConnectionPtr& conn, net::Buffer* buf, gw::Timestamp receive_time) {
    auto* state = stateOf(conn);
    if (!state) return;
    state->pending.append(buf->peek(), buf->readableBytes());
    buf->retrieveAll();

    if (state->keepalive_timer.valid()) {
        conn->getLoop()->cancel(state->keepalive_timer);
        state->keepalive_timer = net::TimerId{};
    }
    if (!state->got_request_line) {
        state->got_request_line = true;
    }
    if (state->busy) {
        // 流式期间下游发来的多余字节只进缓冲；超过 64KB 视为异常
        if (state->pending.readableBytes() > 64 * 1024) {
            SPDLOG_WARN("downstream sent too much data while busy, closing: {}", conn->name());
            conn->forceClose();
        }
        return;
    }
    processBuffer(conn, state);
    (void)receive_time;
}

void HttpServer::finishRequest(const net::TcpConnectionPtr& conn) {
    auto* state = stateOf(conn);
    if (!state) return;
    state->busy = false;
    state->session.reset();
    if (state->draining) {
        conn->shutdown();
        return;
    }
    if (!conn->connected()) return;
    if (state->pending.readableBytes() > 0) {
        state->got_request_line = false;
        processBuffer(conn, state);
        return;
    }
    state->parser.reset();
    state->got_request_line = false;
    startHeaderTimer(conn, state);
    startKeepaliveTimer(conn, state);
}

}  // namespace gw::http
