#include "net/buffer.hpp"

#include <sys/uio.h>

#include <cstring>

namespace gw::net {

Buffer::Buffer(std::size_t initial_size)
    : buffer_(kCheapPrepend + initial_size), reader_index_(kCheapPrepend), writer_index_(kCheapPrepend) {}

const char* Buffer::findCRLF(const char* start, std::size_t len) const {
    if (len < 2) return nullptr;
    const char* pos = static_cast<const char*>(::memmem(start, len, "\r\n", 2));
    return pos;
}

const char* Buffer::findCRLF(const char* start) const {
    const char* end = begin() + writer_index_;
    if (start > end) return nullptr;
    return findCRLF(start, static_cast<std::size_t>(end - start));
}

const char* Buffer::findDoubleCRLF() const {
    if (readableBytes() < 4) return nullptr;
    return static_cast<const char*>(::memmem(peek(), readableBytes(), "\r\n\r\n", 4));
}

const char* Buffer::findEOL() const {
    const void* eol = ::memchr(peek(), '\n', readableBytes());
    return static_cast<const char*>(eol);
}

void Buffer::retrieve(std::size_t n) {
    if (n > readableBytes()) n = readableBytes();
    if (n < readableBytes()) {
        reader_index_ += n;
    } else {
        retrieveAll();
    }
}

void Buffer::retrieveAll() {
    reader_index_ = kCheapPrepend;
    writer_index_ = kCheapPrepend;
}

std::string Buffer::retrieveAllAsString() {
    return retrieveAsString(readableBytes());
}

std::string Buffer::retrieveAsString(std::size_t n) {
    if (n > readableBytes()) n = readableBytes();
    std::string result(peek(), n);
    retrieve(n);
    return result;
}

void Buffer::append(const char* data, std::size_t n) {
    ensureWritableBytes(n);
    std::memcpy(beginWrite(), data, n);
    hasWritten(n);
}

void Buffer::append(const void* data, std::size_t n) {
    append(static_cast<const char*>(data), n);
}

void Buffer::ensureWritableBytes(std::size_t n) {
    if (writableBytes() < n) makeSpace(n);
}

void Buffer::hasWritten(std::size_t n) {
    writer_index_ += n;
}

void Buffer::prepend(const void* data, std::size_t n) {
    if (n > prependableBytes()) return;
    reader_index_ -= n;
    std::memcpy(begin() + reader_index_, data, n);
}

void Buffer::shrink(std::size_t reserve) {
    if (readableBytes() == 0 && buffer_.size() > reserve + kCheapPrepend) {
        buffer_.resize(kCheapPrepend + reserve);
        buffer_.shrink_to_fit();
        reader_index_ = kCheapPrepend;
        writer_index_ = kCheapPrepend;
    }
}

void Buffer::makeSpace(std::size_t len) {
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
        buffer_.resize(writer_index_ + len);
    } else {
        // 前移 readable，腾出 prependable 空间
        std::size_t readable = readableBytes();
        std::memmove(begin() + kCheapPrepend, begin() + reader_index_, readable);
        reader_index_ = kCheapPrepend;
        writer_index_ = reader_index_ + readable;
    }
}

ssize_t Buffer::readFd(int fd, int* saved_errno) {
    char extrabuf[65536];
    struct iovec vec[2];
    const std::size_t writable = writableBytes();
    vec[0].iov_base = begin() + writer_index_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

    ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *saved_errno = errno;
        return n;
    }
    if (static_cast<std::size_t>(n) <= writable) {
        writer_index_ += static_cast<std::size_t>(n);
    } else {
        writer_index_ = buffer_.size();
        append(extrabuf, static_cast<std::size_t>(n) - writable);
    }
    return n;
}

}  // namespace gw::net
