#include "net/inet_address.hpp"

#include <arpa/inet.h>
#include <netdb.h>

#include <cstring>

namespace gw::net {

namespace {

constexpr std::size_t kInet4AddrSize = sizeof(struct sockaddr_in);
constexpr std::size_t kInet6AddrSize = sizeof(struct sockaddr_in6);

}  // namespace

InetAddress::InetAddress(std::uint16_t port, bool loopback_only, bool ipv6) {
    if (ipv6) {
        auto* addr6 = reinterpret_cast<struct sockaddr_in6*>(&storage_);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_addr = loopback_only ? in6addr_loopback : in6addr_any;
        addr6->sin6_port = htons(port);
    } else {
        auto* addr = reinterpret_cast<struct sockaddr_in*>(&storage_);
        addr->sin_family = AF_INET;
        addr->sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
        addr->sin_port = htons(port);
    }
}

InetAddress::InetAddress(std::string ip, std::uint16_t port) {
    if (ip.find(':') != std::string::npos) {
        auto* addr6 = reinterpret_cast<struct sockaddr_in6*>(&storage_);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        ::inet_pton(AF_INET6, ip.c_str(), &addr6->sin6_addr);
    } else {
        auto* addr = reinterpret_cast<struct sockaddr_in*>(&storage_);
        addr->sin_family = AF_INET;
        addr->sin_port = htons(port);
        ::inet_pton(AF_INET, ip.c_str(), &addr->sin_addr);
    }
}

InetAddress::InetAddress(const struct sockaddr_in& addr) {
    std::memcpy(&storage_, &addr, kInet4AddrSize);
}

InetAddress::InetAddress(const struct sockaddr_in6& addr6) {
    std::memcpy(&storage_, &addr6, kInet6AddrSize);
}

const struct sockaddr* InetAddress::sockaddr() const {
    return reinterpret_cast<const struct sockaddr*>(&storage_);
}

struct sockaddr* InetAddress::sockaddr() {
    return reinterpret_cast<struct sockaddr*>(&storage_);
}

socklen_t InetAddress::sockaddrLen() const {
    return family() == AF_INET6 ? static_cast<socklen_t>(kInet6AddrSize) : static_cast<socklen_t>(kInet4AddrSize);
}

std::string InetAddress::toIp() const {
    char buf[INET6_ADDRSTRLEN] = {};
    if (family() == AF_INET) {
        const auto* addr = reinterpret_cast<const struct sockaddr_in*>(&storage_);
        ::inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
    } else if (family() == AF_INET6) {
        const auto* addr6 = reinterpret_cast<const struct sockaddr_in6*>(&storage_);
        ::inet_ntop(AF_INET6, &addr6->sin6_addr, buf, sizeof(buf));
    }
    return buf;
}

std::string InetAddress::toIpPort() const {
    std::string ip = toIp();
    std::uint16_t p = port();
    if (family() == AF_INET6) return "[" + ip + "]:" + std::to_string(p);
    return ip + ":" + std::to_string(p);
}

std::uint16_t InetAddress::port() const {
    if (family() == AF_INET) {
        const auto* addr = reinterpret_cast<const struct sockaddr_in*>(&storage_);
        return ntohs(addr->sin_port);
    }
    if (family() == AF_INET6) {
        const auto* addr6 = reinterpret_cast<const struct sockaddr_in6*>(&storage_);
        return ntohs(addr6->sin6_port);
    }
    return 0;
}

void InetAddress::setScopeId(std::uint32_t scope_id) {
    if (family() == AF_INET6) {
        auto* addr6 = reinterpret_cast<struct sockaddr_in6*>(&storage_);
        addr6->sin6_scope_id = scope_id;
    }
}

}  // namespace gw::net
