#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>

namespace gw::net {

class InetAddress {
public:
    InetAddress() = default;
    explicit InetAddress(std::uint16_t port, bool loopback_only = false, bool ipv6 = false);
    InetAddress(std::string ip, std::uint16_t port);
    explicit InetAddress(const struct sockaddr_in& addr);
    explicit InetAddress(const struct sockaddr_in6& addr6);

    sa_family_t family() const { return storage_.ss_family; }
    const struct sockaddr* sockaddr() const;
    struct sockaddr* sockaddr();
    socklen_t sockaddrLen() const;

    std::string toIp() const;
    std::string toIpPort() const;
    std::uint16_t port() const;

    void setScopeId(std::uint32_t scope_id);

private:
    struct sockaddr_storage storage_ {};
};

}  // namespace gw::net
