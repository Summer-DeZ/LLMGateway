#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gw::net {

// prependable(8) | readable | writable
class Buffer {
public:
    static constexpr std::size_t kCheapPrepend = 8;
    static constexpr std::size_t kInitialSize = 1024;

    explicit Buffer(std::size_t initial_size = kInitialSize);

    std::size_t readableBytes() const { return writer_index_ - reader_index_; }
    std::size_t writableBytes() const { return buffer_.size() - writer_index_; }
    std::size_t prependableBytes() const { return reader_index_; }

    const char* peek() const { return begin() + reader_index_; }
    const char* beginWrite() const { return begin() + writer_index_; }
    char* beginWrite() { return begin() + writer_index_; }

    const char* findCRLF() const { return findCRLF(peek(), readableBytes()); }
    const char* findCRLF(const char* start) const;
    const char* findDoubleCRLF() const;
    const char* findEOL() const;

    void retrieve(std::size_t n);
    void retrieveAll();
    std::string retrieveAllAsString();
    std::string retrieveAsString(std::size_t n);

    void append(const char* data, std::size_t n);
    void append(std::string_view s) { append(s.data(), s.size()); }
    void append(const void* data, std::size_t n);

    void ensureWritableBytes(std::size_t n);
    void hasWritten(std::size_t n);

    void prepend(const void* data, std::size_t n);
    void shrink(std::size_t reserve = kInitialSize);

    // readv 两段：可写区 + 栈上 64KB；返回读到的字节数，-1 时 *saved_errno 有效。
    ssize_t readFd(int fd, int* saved_errno);

private:
    char* begin() { return buffer_.data(); }
    const char* begin() const { return buffer_.data(); }
    const char* findCRLF(const char* start, std::size_t len) const;
    void makeSpace(std::size_t len);

    std::vector<char> buffer_;
    std::size_t reader_index_;
    std::size_t writer_index_;
};

}  // namespace gw::net
