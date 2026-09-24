#include "net/socket.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "base/log.hpp"
#include "net/inet_address.hpp"

namespace gw::net {

Socket::~Socket() {
    ::close(sockfd_);
}

int Socket::release() {
    return const_cast<int&>(sockfd_);
}

int Socket::createNonblocking(sa_family_t family) {
    int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    return fd;
}

void Socket::bindAddress(const InetAddress& local_addr) {
    int ret = ::bind(sockfd_, local_addr.sockaddr(), local_addr.sockaddrLen());
    if (ret < 0) {
        SPDLOG_CRITICAL("bind {} failed: {}", local_addr.toIpPort(), std::strerror(errno));
        abort();
    }
}

void Socket::listen() {
    int ret = ::listen(sockfd_, SOMAXCONN);
    if (ret < 0) {
        SPDLOG_CRITICAL("listen failed: {}", std::strerror(errno));
        abort();
    }
}

int Socket::accept(InetAddress* peer, int* saved_errno) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    std::memset(&addr, 0, sizeof(addr));
    int connfd = ::accept4(sockfd_, reinterpret_cast<struct sockaddr*>(&addr), &len,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0) {
        if (addr.ss_family == AF_INET) {
            *peer = InetAddress(*reinterpret_cast<struct sockaddr_in*>(&addr));
        } else if (addr.ss_family == AF_INET6) {
            *peer = InetAddress(*reinterpret_cast<struct sockaddr_in6*>(&addr));
        }
    } else {
        *saved_errno = errno;
    }
    return connfd;
}

void Socket::shutdownWrite() {
    if (::shutdown(sockfd_, SHUT_WR) < 0) {
        SPDLOG_DEBUG("shutdownWrite failed: {}", std::strerror(errno));
    }
}

void Socket::setTcpNoDelay(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

void Socket::setReuseAddr(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

void Socket::setReusePort(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

void Socket::setKeepAlive(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}

}  // namespace gw::net
