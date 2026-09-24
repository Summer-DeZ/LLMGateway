#include "net/buffer.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <string>
#include <thread>

using gw::net::Buffer;

TEST(BufferTest, AppendRetrieve) {
    Buffer buf;
    EXPECT_EQ(buf.readableBytes(), 0u);
    buf.append("hello ", 6);
    buf.append(std::string_view("world"));
    EXPECT_EQ(buf.readableBytes(), 11u);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), "hello world");
    EXPECT_EQ(buf.retrieveAsString(5), "hello");
    EXPECT_EQ(buf.readableBytes(), 6u);
    EXPECT_EQ(buf.retrieveAllAsString(), " world");
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(BufferTest, Prepend) {
    Buffer buf;
    buf.append("world", 5);
    buf.prepend("hello ", 6);
    EXPECT_EQ(buf.retrieveAllAsString(), "hello world");
    EXPECT_EQ(buf.prependableBytes(), Buffer::kCheapPrepend);
}

TEST(BufferTest, EnsureWritableMoveThenGrow) {
    Buffer buf(16);
    buf.append(std::string(16, 'a'));
    EXPECT_EQ(buf.writableBytes(), 0u);
    buf.retrieve(10);
    // 前移可以满足
    buf.ensureWritableBytes(5);
    EXPECT_EQ(buf.prependableBytes(), Buffer::kCheapPrepend);
    EXPECT_EQ(buf.readableBytes(), 6u);
    buf.append(std::string(10, 'b'));
    EXPECT_EQ(buf.readableBytes(), 16u);
    // 需要扩容
    buf.ensureWritableBytes(100);
    EXPECT_GE(buf.writableBytes(), 100u);
    EXPECT_EQ(std::string(buf.peek(), 6), std::string(6, 'a'));
}

TEST(BufferTest, FindCRLFAcrossGrow) {
    Buffer buf(8);
    buf.append(std::string(100, 'x'));
    EXPECT_EQ(buf.findCRLF(), nullptr);
    buf.append("\r\n", 2);
    const char* crlf = buf.findCRLF();
    ASSERT_NE(crlf, nullptr);
    EXPECT_EQ(static_cast<std::size_t>(crlf - buf.peek()), 100u);
    EXPECT_EQ(buf.findDoubleCRLF(), nullptr);
    buf.append("\r\n", 2);
    EXPECT_NE(buf.findDoubleCRLF(), nullptr);
}

TEST(BufferTest, FindEOLAndDoubleCRLF) {
    Buffer buf;
    buf.append("GET / HTTP/1.1\r\nHost: x\r\n\r\nBODY", 31);
    const char* d = buf.findDoubleCRLF();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(static_cast<std::size_t>(d - buf.peek()), 23u);
    EXPECT_NE(buf.findEOL(), nullptr);
}

TEST(BufferTest, ReadFdLarge) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const std::string payload(100 * 1024, 'z');
    std::thread writer([&] {
        std::size_t off = 0;
        while (off < payload.size()) {
            ssize_t w = ::write(fds[1], payload.data() + off, payload.size() - off);
            ASSERT_GT(w, 0);
            off += static_cast<std::size_t>(w);
        }
        ::close(fds[1]);
    });

    Buffer buf;
    std::size_t total = 0;
    while (total < payload.size()) {
        int saved = 0;
        ssize_t n = buf.readFd(fds[0], &saved);
        ASSERT_GT(n, 0) << "saved errno=" << saved;
        total += static_cast<std::size_t>(n);
    }
    writer.join();
    EXPECT_EQ(buf.readableBytes(), payload.size());
    EXPECT_EQ(std::string(buf.peek(), 10), std::string(10, 'z'));
    ::close(fds[0]);
}

TEST(BufferTest, ReadFdWouldBlock) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    int flags = ::fcntl(fds[0], F_GETFL, 0);
    ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    Buffer buf;
    int saved = 0;
    ssize_t n = buf.readFd(fds[0], &saved);
    EXPECT_EQ(n, -1);
    EXPECT_EQ(saved, EAGAIN);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(BufferTest, Shrink) {
    Buffer buf;
    buf.append(std::string(100000, 'a'));
    buf.retrieveAll();
    buf.shrink();
    EXPECT_LE(buf.writableBytes(), Buffer::kInitialSize + 1);
}
