#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "net/inet_address.hpp"

namespace gw::net {

class EventLoop;

// 单工作线程 + 缓存；结果经 loop->queueInLoop 回到发起 loop。
class Resolver : noncopyable {
public:
    using Result = std::expected<std::vector<InetAddress>, std::string>;

    explicit Resolver(std::chrono::seconds cache_ttl = std::chrono::seconds{60});
    ~Resolver();

    // cb 在 loop 线程上执行；host 是 IP 字面量时不进入工作线程
    void resolve(std::string host, std::uint16_t port, EventLoop* loop, std::function<void(Result)> cb);

    void stop();

private:
    struct CacheEntry {
        std::vector<InetAddress> addrs;
        std::string error;
        Timestamp expires;
    };
    struct Task {
        std::string host;
        std::uint16_t port;
        EventLoop* loop;
        std::function<void(Result)> cb;
    };

    void workerLoop();
    void postResult(EventLoop* loop, std::function<void(Result)> cb, Result result);

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<Task> queue_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::chrono::seconds cache_ttl_;
    bool stopping_ = false;
};

}  // namespace gw::net
