#include "net/transport.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace gw::net {

PlainTransport::~PlainTransport() {
    if (fd_ >= 0) ::close(fd_);
}

IoResult PlainTransport::readInto(Buffer& in) {
    int saved_errno = 0;
    ssize_t n = in.readFd(fd_, &saved_errno);
    if (n > 0) {
        return {IoStatus::ok, static_cast<std::size_t>(n)};
    }
    if (n == 0) {
        return {IoStatus::eof, 0};
    }
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK || saved_errno == EINTR) {
        return {IoStatus::would_block, 0};
    }
    last_error_ = std::strerror(saved_errno);
    return {IoStatus::error, 0};
}

IoResult PlainTransport::write(const char* data, std::size_t n) {
    ssize_t written = ::write(fd_, data, n);
    if (written >= 0) {
        return {IoStatus::ok, static_cast<std::size_t>(written)};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {IoStatus::would_block, 0};
    }
    if (errno == EINTR) {
        return {IoStatus::ok, 0};
    }
    last_error_ = std::strerror(errno);
    return {IoStatus::error, 0};
}

void PlainTransport::shutdownWrite() {
    ::shutdown(fd_, SHUT_WR);
}

}  // namespace gw::net
