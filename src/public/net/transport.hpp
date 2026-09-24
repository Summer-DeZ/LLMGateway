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

class Transport {
public:
    virtual ~Transport() = default;

    // 读到 would_block/eof/error 或本轮上限（64KB）为止
    virtual IoResult readInto(Buffer& in) = 0;
    virtual IoResult write(const char* data, std::size_t n) = 0;
    virtual void shutdownWrite() = 0;
    virtual std::string lastError() const = 0;
};

class PlainTransport : public Transport {
public:
    // 接管 fd 的所有权，析构时关闭
    explicit PlainTransport(int fd) : fd_(fd) {}
    ~PlainTransport() override;

    IoResult readInto(Buffer& in) override;
    IoResult write(const char* data, std::size_t n) override;
    void shutdownWrite() override;
    std::string lastError() const override { return last_error_; }

    int fd() const { return fd_; }

private:
    int fd_;
    std::string last_error_;
};

}  // namespace gw::net
