#include "http/upstream_client.hpp"

#include "base/log.hpp"

namespace gw::http {

UpstreamExchange::UpstreamExchange(Lease lease, const UrlParts& url, UpstreamRequest request, Handler handler)
    : lease_(std::move(lease)),
      url_(url),
      request_(std::move(request)),
      handler_(std::move(handler)),
      parser_(
          ResponseParserLimits{},
          ResponseParser::Callbacks{
              .on_head =
                  [this](int status, const Headers& headers) {
                      head_received_ = true;
                      status_ = status;
                      response_headers_ = headers;
                      if (handler_.on_head) handler_.on_head(status, headers);
                  },
              .on_data =
                  [this](std::string_view data) {
                      if (handler_.on_body) handler_.on_body(data);
                  },
              .on_complete =
                  [this](bool reusable) {
                      reusable_ = reusable;
                      finished_ = true;
                      if (handler_.on_complete) handler_.on_complete(reusable);
                  },
              .on_error =
                  [this](std::string message) {
                      finished_ = true;
                      reusable_ = false;
                      if (handler_.on_error) handler_.on_error(UpstreamIoError{UpstreamIoError::Kind::parse_error, message});
                  }}) {}

UpstreamExchange::~UpstreamExchange() = default;

void UpstreamExchange::buildRequest() {
    std::string out;
    std::string host_header = url_.host;
    if (url_.port != (url_.tls ? 443 : 80)) host_header += ":" + std::to_string(url_.port);

    out.append(request_.method);
    out.push_back(' ');
    out.append(request_.path);
    out.append(" HTTP/1.1\r\n");
    out.append("Host: " + host_header + "\r\n");
    out.append("Content-Length: " + std::to_string(request_.body.size()) + "\r\n");
    out.append("Connection: keep-alive\r\n");
    out.append("User-Agent: llm-gateway/0.1\r\n");
    for (const auto& [name, value] : request_.headers) {
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }
    out.append("\r\n");
    out.append(request_.body);

    auto conn = lease_.conn;
    conn->send(out);
}

void UpstreamExchange::start() {
    auto conn = lease_.conn;
    std::weak_ptr<UpstreamExchange> weak = shared_from_this();
    conn->setMessageCallback([weak](const net::TcpConnectionPtr& c, net::Buffer* buf, gw::Timestamp) {
        auto self = weak.lock();
        if (!self) return;
        self->handleMessage(c, buf);
    });
    conn->setConnectionCallback([weak](const net::TcpConnectionPtr& c) {
        if (c->connected()) return;
        auto self = weak.lock();
        if (!self) return;
        self->handleClose(c);
    });
    conn->setWriteCompleteCallback(nullptr);
    conn->setCloseCallback(nullptr);

    request_written_ = nowSteady();
    buildRequest();
}

void UpstreamExchange::handleMessage(const net::TcpConnectionPtr& conn, net::Buffer* buf) {
    (void)conn;
    if (!received_any_byte_ && buf->readableBytes() > 0) {
        received_any_byte_ = true;
        first_byte_time_ = nowSteady();
        ttfb_ = first_byte_time_ - request_written_;
    }
    parser_.feed(*buf);
}

void UpstreamExchange::handleClose(const net::TcpConnectionPtr& conn) {
    (void)conn;
    if (finished_ || draining_) return;
    if (!head_received_) {
        finished_ = true;
        reusable_ = false;
        if (handler_.on_error) {
            handler_.on_error(UpstreamIoError{UpstreamIoError::Kind::closed_before_head,
                                              "upstream closed before response head"});
        }
        return;
    }
    if (!parser_.complete()) {
        parser_.feedEof();
        if (parser_.complete()) return;
        finished_ = true;
        reusable_ = false;
        if (handler_.on_error) {
            handler_.on_error(UpstreamIoError{UpstreamIoError::Kind::closed_mid_body,
                                              "upstream closed before response complete"});
        }
    }
}

void UpstreamExchange::pauseReading() {
    if (lease_.conn) lease_.conn->stopRead();
}

void UpstreamExchange::resumeReading() {
    if (lease_.conn) lease_.conn->startRead();
}

void UpstreamExchange::abortAndDrain(const std::function<void(std::string_view)>& on_drain,
                                    const std::function<void()>& on_shutdown) {
    draining_ = true;
    finished_ = true;
    reusable_ = false;
    if (!lease_.conn) return;
    lease_.conn->abortAndDrain([this, on_drain](std::string_view data) {
        if (!received_any_byte_ && !data.empty()) {
            received_any_byte_ = true;
            first_byte_time_ = nowSteady();
            ttfb_ = first_byte_time_ - request_written_;
        }
        if (on_drain) on_drain(data);
    }, 1 << 20, on_shutdown);
}

void UpstreamExchange::forceClose() {
    finished_ = true;
    reusable_ = false;
    if (lease_.conn) lease_.conn->forceClose();
}

}  // namespace gw::http
