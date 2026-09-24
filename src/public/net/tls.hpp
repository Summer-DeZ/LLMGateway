#pragma once

#include <openssl/ssl.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "base/noncopyable.hpp"
#include "net/transport.hpp"

namespace gw::net {

// 整个进程一个，跨线程只读共享（内部会话缓存有锁）
class TlsContext : noncopyable {
public:
    static std::expected<std::unique_ptr<TlsContext>, std::string> create(bool verify_peer, const std::string& ca_file);
    ~TlsContext();

    std::expected<std::unique_ptr<Transport>, std::string> createTransport(int fd, const std::string& host,
                                                                           std::uint16_t port);

    SSL_CTX* raw() const { return ctx_; }
    void storeSession(const std::string& cache_key, SSL_SESSION* session);
    SSL_SESSION* takeSession(const std::string& cache_key);

private:
    TlsContext() = default;

    SSL_CTX* ctx_ = nullptr;
    std::mutex session_mutex_;
    std::unordered_map<std::string, SSL_SESSION*> sessions_;
};

// 每连接一个，只在所属 loop 线程使用
class TlsTransport : public Transport {
public:
    TlsTransport(TlsContext* context, SSL* ssl, std::string host, std::uint16_t port, int fd);
    ~TlsTransport() override;

    HandshakeStatus handshake() override;
    IoResult readInto(Buffer& in) override;
    IoResult write(const char* data, std::size_t n) override;
    bool hasBufferedInput() const override;
    bool readWantsWrite() const override { return read_wants_write_; }
    bool writeWantsRead() const override { return write_wants_read_; }
    void shutdownWrite() override;
    std::string lastError() const override { return last_error_; }
    bool tlsResumed() const override { return resumed_; }

    // 会话缓存键（host:port）的存储地址，交给 SSL ex_data 供 new-session 回调使用
    std::string* sessionKey() { return &cache_key_; }

private:
    TlsContext* context_;
    SSL* ssl_;
    std::string host_;
    std::uint16_t port_;
    std::string cache_key_;
    int fd_;
    bool read_wants_write_ = false;
    bool write_wants_read_ = false;
    bool resumed_ = false;
    std::string last_error_;
};

}  // namespace gw::net
