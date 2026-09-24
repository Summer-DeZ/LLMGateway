#pragma once

#include <cstddef>
#include <string>

#include "net/buffer.hpp"

namespace gw::net {

enum class IoStatus { ok, would_block, eof, error };

struct IoResult {
    IoStatus status = IoStatus::ok;
    std::size_t bytes = 0;
};

enum class HandshakeStatus { done, want_read, want_write, failed };

class Transport {
public:
    virtual ~Transport() = default;

    virtual HandshakeStatus handshake() = 0;
    // 读到 would_block/eof/error 或本轮上限（64KB）为止
    virtual IoResult readInto(Buffer& in) = 0;
    virtual IoResult write(const char* data, std::size_t n) = 0;
    virtual bool hasBufferedInput() const = 0;
    virtual bool readWantsWrite() const { return false; }
    virtual bool writeWantsRead() const { return false; }
    virtual void shutdownWrite() = 0;
    virtual std::string lastError() const = 0;
    virtual bool tlsResumed() const { return false; }
};

class PlainTransport : public Transport {
public:
    explicit PlainTransport(int fd) : fd_(fd) {}

    HandshakeStatus handshake() override { return HandshakeStatus::done; }
    IoResult readInto(Buffer& in) override;
    IoResult write(const char* data, std::size_t n) override;
    bool hasBufferedInput() const override { return false; }
    void shutdownWrite() override;
    std::string lastError() const override { return last_error_; }

    int fd() const { return fd_; }

private:
    int fd_;
    std::string last_error_;
};

}  // namespace gw::net
