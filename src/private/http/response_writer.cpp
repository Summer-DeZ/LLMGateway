#include "http/response_writer.hpp"

#include <ctime>

#include "base/timestamp.hpp"

namespace gw::http {

namespace {

constexpr std::string_view kServerName = "llm-gateway/0.1";

void appendHeader(std::string& out, std::string_view name, std::string_view value) {
    out.append(name);
    out.append(": ");
    out.append(value);
    out.append("\r\n");
}

char hexDigit(unsigned v) {
    return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10));
}

}  // namespace

std::string reasonPhrase(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

std::string httpDateNow() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

ResponseWriter::ResponseWriter(const std::weak_ptr<net::TcpConnection>& conn, std::string request_id, bool keep_alive)
    : conn_(conn), request_id_(std::move(request_id)), keep_alive_(keep_alive) {}

std::shared_ptr<net::TcpConnection> ResponseWriter::lockConn() const {
    return conn_.lock();
}

std::size_t ResponseWriter::pendingBytes() const {
    auto conn = lockConn();
    return conn ? conn->outputBytes() : 0;
}

void ResponseWriter::writeHead(int status, const Headers& headers, std::string_view content_type, bool chunked,
                               std::optional<std::size_t> content_length) {
    std::string out;
    out.reserve(256);
    out.append("HTTP/1.1 ");
    out.append(std::to_string(status));
    out.push_back(' ');
    out.append(reasonPhrase(status));
    out.append("\r\n");
    appendHeader(out, "server", kServerName);
    appendHeader(out, "date", httpDateNow());
    if (!request_id_.empty()) appendHeader(out, "x-request-id", request_id_);
    if (!content_type.empty()) appendHeader(out, "content-type", content_type);
    if (chunked) {
        appendHeader(out, "transfer-encoding", "chunked");
    } else if (content_length) {
        appendHeader(out, "content-length", std::to_string(*content_length));
    }
    appendHeader(out, "connection", keep_alive_ ? "keep-alive" : "close");
    for (const auto& [name, value] : headers) {
        appendHeader(out, name, value);
    }
    out.append("\r\n");

    auto conn = lockConn();
    if (conn) conn->send(out);
    committed_ = true;
}

void ResponseWriter::writeFull(int status, const Headers& headers, std::string_view content_type,
                               std::string_view body) {
    writeHead(status, headers, content_type, false, body.size());
    auto conn = lockConn();
    if (conn && !body.empty()) conn->send(body);
}

void ResponseWriter::writeStreamHead(int status, const Headers& headers) {
    Headers all = headers;
    bool has_cache_control = false;
    for (const auto& [name, value] : all) {
        if (name == "cache-control") has_cache_control = true;
    }
    if (!has_cache_control) all.emplace_back("cache-control", "no-cache");
    all.emplace_back("x-accel-buffering", "no");
    writeHead(status, all, "text/event-stream", true, std::nullopt);
}

void ResponseWriter::writeChunk(std::string_view data) {
    if (data.empty()) return;
    std::string out;
    out.reserve(data.size() + 16);
    std::size_t n = data.size();
    char digits[17];
    int i = 0;
    if (n == 0) {
        digits[i++] = '0';
    }
    while (n > 0) {
        digits[i++] = hexDigit(static_cast<unsigned>(n & 0xF));
        n >>= 4;
    }
    while (i > 0) out.push_back(digits[--i]);
    out.append("\r\n");
    out.append(data);
    out.append("\r\n");
    auto conn = lockConn();
    if (conn) conn->send(out);
}

void ResponseWriter::endStream() {
    auto conn = lockConn();
    if (conn) conn->send("0\r\n\r\n");
}

}  // namespace gw::http
