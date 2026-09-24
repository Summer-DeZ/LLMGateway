#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "http/http_request.hpp"
#include "net/tcp_connection.hpp"

namespace gw::http {

class ResponseWriter {
public:
    ResponseWriter(const std::weak_ptr<net::TcpConnection>& conn, std::string request_id, bool keep_alive);

    void writeFull(int status, const Headers& headers, std::string_view content_type, std::string_view body);
    void writeStreamHead(int status, const Headers& headers);
    void writeChunk(std::string_view data);
    void endStream();

    bool committed() const { return committed_; }
    std::size_t pendingBytes() const;

    void setKeepAlive(bool keep_alive) { keep_alive_ = keep_alive; }

private:
    void writeHead(int status, const Headers& headers, std::string_view content_type, bool chunked,
                   std::optional<std::size_t> content_length);
    std::shared_ptr<net::TcpConnection> lockConn() const;

    std::weak_ptr<net::TcpConnection> conn_;
    std::string request_id_;
    bool keep_alive_;
    bool committed_ = false;
};

std::string reasonPhrase(int status);
std::string httpDateNow();

}  // namespace gw::http
