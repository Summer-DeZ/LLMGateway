#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "http/http_request.hpp"
#include "http/response_parser.hpp"
#include "http/upstream_pool.hpp"
#include "http/url.hpp"

namespace gw::http {

struct UpstreamRequest {
    std::string method = "POST";
    std::string path;
    Headers headers;
    std::string body;
};

struct UpstreamIoError {
    enum class Kind { closed_before_head, closed_mid_body, parse_error, write_error, cancelled };
    Kind kind = Kind::closed_before_head;
    std::string message;
};

// 对一个已获得的 Lease 发送一次请求并增量接收响应
class UpstreamExchange : noncopyable, public std::enable_shared_from_this<UpstreamExchange> {
public:
    struct Handler {
        std::function<void(int status, const Headers&)> on_head;
        std::function<void(std::string_view)> on_body;
        std::function<void(bool reusable)> on_complete;
        std::function<void(UpstreamIoError)> on_error;
    };

    UpstreamExchange(Lease lease, const UrlParts& url, UpstreamRequest request, Handler handler);
    ~UpstreamExchange();

    void start();
    void pauseReading();
    void resumeReading();

    // SHUT_WR → 非阻塞读完已到达的字节交给 on_drain（只用于计量）→ close；之后不再回调
    void abortAndDrain(const std::function<void(std::string_view)>& on_drain,
                       const std::function<void()>& on_shutdown = {});
    void forceClose();

    bool receivedAnyByte() const { return received_any_byte_; }
    bool headReceived() const { return head_received_; }
    bool finished() const { return finished_; }
    int status() const { return status_; }
    const Headers& responseHeaders() const { return response_headers_; }
    bool reusable() const { return reusable_; }
    Duration upstreamTtfb() const { return ttfb_; }
    const Lease& lease() const { return lease_; }
    Lease& lease() { return lease_; }

private:
    void handleMessage(const net::TcpConnectionPtr& conn, net::Buffer* buf);
    void handleClose(const net::TcpConnectionPtr& conn);
    void buildRequest();

    Lease lease_;
    UrlParts url_;
    UpstreamRequest request_;
    Handler handler_;
    ResponseParser parser_;
    Timestamp request_written_;
    Timestamp first_byte_time_;
    Duration ttfb_{};
    bool received_any_byte_ = false;
    bool head_received_ = false;
    bool finished_ = false;
    bool draining_ = false;
    int status_ = 0;
    Headers response_headers_;
    bool reusable_ = false;
};

}  // namespace gw::http
