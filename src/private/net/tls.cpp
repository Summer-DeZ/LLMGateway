#include "net/tls.hpp"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "base/log.hpp"

namespace gw::net {

namespace {

int g_host_ex_index = -1;

std::string sslErrorText(SSL* ssl) {
    unsigned long err = ERR_get_error();
    std::string text;
    if (err != 0) {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        text = buf;
    } else {
        text = "SSL error";
    }
    long verify = SSL_get_verify_result(ssl);
    if (verify != X509_V_OK) {
        text += ": ";
        text += X509_verify_cert_error_string(verify);
    }
    return text;
}

bool isIpLiteral(const std::string& host) {
    in_addr addr4{};
    in6_addr addr6{};
    return ::inet_pton(AF_INET, host.c_str(), &addr4) == 1 || ::inet_pton(AF_INET6, host.c_str(), &addr6) == 1;
}

int newSessionCallback(SSL* ssl, SSL_SESSION* session) {
    auto* context = static_cast<TlsContext*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
    auto* key = static_cast<std::string*>(SSL_get_ex_data(ssl, g_host_ex_index));
    if (context != nullptr && key != nullptr) {
        context->storeSession(*key, session);
        return 1;  // 接管引用
    }
    return 0;
}

}  // namespace

std::expected<std::unique_ptr<TlsContext>, std::string> TlsContext::create(bool verify_peer,
                                                                           const std::string& ca_file) {
    if (g_host_ex_index < 0) {
        g_host_ex_index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    }
    auto context = std::unique_ptr<TlsContext>(new TlsContext());
    context->ctx_ = SSL_CTX_new(TLS_client_method());
    if (context->ctx_ == nullptr) return std::unexpected("SSL_CTX_new failed");
    SSL_CTX_set_app_data(context->ctx_, context.get());
    SSL_CTX_set_min_proto_version(context->ctx_, TLS1_2_VERSION);
    if (verify_peer) {
        SSL_CTX_set_verify(context->ctx_, SSL_VERIFY_PEER, nullptr);
        if (!SSL_CTX_set_default_verify_paths(context->ctx_)) {
            return std::unexpected("SSL_CTX_set_default_verify_paths failed");
        }
        if (!ca_file.empty() && SSL_CTX_load_verify_locations(context->ctx_, ca_file.c_str(), nullptr) != 1) {
            return std::unexpected("SSL_CTX_load_verify_locations failed: " + ca_file);
        }
    } else {
        SSL_CTX_set_verify(context->ctx_, SSL_VERIFY_NONE, nullptr);
    }
    static const unsigned char kAlpn[] = {0x08, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    SSL_CTX_set_alpn_protos(context->ctx_, kAlpn, sizeof(kAlpn));
    SSL_CTX_set_session_cache_mode(context->ctx_, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(context->ctx_, newSessionCallback);
    return context;
}

TlsContext::~TlsContext() {
    for (auto& [key, session] : sessions_) {
        SSL_SESSION_free(session);
    }
    if (ctx_ != nullptr) SSL_CTX_free(ctx_);
}

void TlsContext::storeSession(const std::string& cache_key, SSL_SESSION* session) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto it = sessions_.find(cache_key);
    if (it != sessions_.end()) {
        SSL_SESSION_free(it->second);
        it->second = session;
    } else {
        sessions_.emplace(cache_key, session);
    }
}

SSL_SESSION* TlsContext::takeSession(const std::string& cache_key) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto it = sessions_.find(cache_key);
    if (it == sessions_.end()) return nullptr;
    SSL_SESSION_up_ref(it->second);
    return it->second;
}

std::expected<std::unique_ptr<Transport>, std::string> TlsContext::createTransport(int fd, const std::string& host,
                                                                                   std::uint16_t port) {
    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr) return std::unexpected("SSL_new failed");
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);
    if (!isIpLiteral(host)) {
        SSL_set_tlsext_host_name(ssl, host.c_str());
        SSL_set1_host(ssl, host.c_str());
    } else {
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str());
    }
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    std::string cache_key = host + ":" + std::to_string(port);
    if (SSL_SESSION* session = takeSession(cache_key)) {
        SSL_set_session(ssl, session);
        SSL_SESSION_free(session);
    }
    auto transport = std::make_unique<TlsTransport>(this, ssl, host, port, fd);
    transport->sessionKey()->assign(cache_key);
    SSL_set_ex_data(ssl, g_host_ex_index, transport->sessionKey());
    return transport;
}

TlsTransport::TlsTransport(TlsContext* context, SSL* ssl, std::string host, std::uint16_t port, int fd)
    : context_(context), ssl_(ssl), host_(std::move(host)), port_(port), fd_(fd) {}

TlsTransport::~TlsTransport() {
    if (ssl_ != nullptr) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}

HandshakeStatus TlsTransport::handshake() {
    ERR_clear_error();
    int ret = SSL_do_handshake(ssl_);
    if (ret == 1) {
        resumed_ = SSL_session_reused(ssl_) == 1;
        return HandshakeStatus::done;
    }
    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_READ) return HandshakeStatus::want_read;
    if (err == SSL_ERROR_WANT_WRITE) return HandshakeStatus::want_write;
    last_error_ = sslErrorText(ssl_);
    return HandshakeStatus::failed;
}

IoResult TlsTransport::readInto(Buffer& in) {
    read_wants_write_ = false;
    constexpr std::size_t kChunk = 16 * 1024;
    constexpr std::size_t kLimit = 64 * 1024;
    std::size_t total = 0;
    while (total < kLimit) {
        in.ensureWritableBytes(kChunk);
        std::size_t nread = 0;
        ERR_clear_error();
        int ret = SSL_read_ex(ssl_, in.beginWrite(), in.writableBytes(), &nread);
        if (ret == 1) {
            in.hasWritten(nread);
            total += nread;
            if (nread == 0) break;
            continue;
        }
        int err = SSL_get_error(ssl_, ret);
        if (err == SSL_ERROR_WANT_READ) {
            return total > 0 ? IoResult{IoStatus::ok, total} : IoResult{IoStatus::would_block, 0};
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            read_wants_write_ = true;
            return total > 0 ? IoResult{IoStatus::ok, total} : IoResult{IoStatus::would_block, 0};
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return total > 0 ? IoResult{IoStatus::ok, total} : IoResult{IoStatus::eof, 0};
        }
        if (err == SSL_ERROR_SYSCALL) {
            last_error_ = "unexpected EOF while reading";
            return total > 0 ? IoResult{IoStatus::ok, total} : IoResult{IoStatus::eof, 0};
        }
        last_error_ = sslErrorText(ssl_);
        return total > 0 ? IoResult{IoStatus::ok, total} : IoResult{IoStatus::error, 0};
    }
    return {IoStatus::ok, total};
}

IoResult TlsTransport::write(const char* data, std::size_t n) {
    write_wants_read_ = false;
    std::size_t written = 0;
    ERR_clear_error();
    int ret = SSL_write_ex(ssl_, data, n, &written);
    if (ret == 1) return {IoStatus::ok, written};
    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_WRITE) return {IoStatus::would_block, written};
    if (err == SSL_ERROR_WANT_READ) {
        write_wants_read_ = true;
        return {IoStatus::would_block, written};
    }
    last_error_ = sslErrorText(ssl_);
    return {IoStatus::error, written};
}

bool TlsTransport::hasBufferedInput() const {
    return SSL_has_pending(ssl_) == 1;
}

void TlsTransport::shutdownWrite() {
    ERR_clear_error();
    SSL_shutdown(ssl_);
    ::shutdown(fd_, SHUT_WR);
}

}  // namespace gw::net
