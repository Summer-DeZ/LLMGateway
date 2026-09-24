#include "net/resolver.hpp"

#include <arpa/inet.h>
#include <netdb.h>

#include <cstring>

#include "base/log.hpp"
#include "net/event_loop.hpp"

namespace gw::net {

namespace {

bool isIpLiteral(const std::string& host) {
    in_addr addr4{};
    in6_addr addr6{};
    return ::inet_pton(AF_INET, host.c_str(), &addr4) == 1 || ::inet_pton(AF_INET6, host.c_str(), &addr6) == 1;
}

constexpr auto kFailureCacheTtl = std::chrono::seconds{5};

}  // namespace

Resolver::Resolver(std::chrono::seconds cache_ttl) : cache_ttl_(cache_ttl) {
    thread_ = std::thread([this] { workerLoop(); });
}

Resolver::~Resolver() {
    stop();
}

void Resolver::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        queue_.clear();
    }
    cond_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Resolver::postResult(EventLoop* loop, std::function<void(Result)> cb, Result result) {
    // runInLoop：调用方就在 loop 线程时（例如 IP 字面量）直接执行，不经过一轮 poll
    loop->runInLoop([cb = std::move(cb), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void Resolver::resolve(std::string host, std::uint16_t port, EventLoop* loop, std::function<void(Result)> cb) {
    if (isIpLiteral(host)) {
        std::vector<InetAddress> addrs;
        addrs.emplace_back(host, port);
        postResult(loop, std::move(cb), Result(std::move(addrs)));
        return;
    }

    std::string key = host + ":" + std::to_string(port);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            if (it->second.expires > nowSteady()) {
                SPDLOG_DEBUG("resolver: cache hit for {}", key);
                if (it->second.error.empty()) {
                    postResult(loop, std::move(cb), Result(it->second.addrs));
                } else {
                    postResult(loop, std::move(cb), std::unexpected(it->second.error));
                }
                return;
            }
            cache_.erase(it);
        }
        queue_.push_back(Task{std::move(host), port, loop, std::move(cb)});
    }
    cond_.notify_one();
}

void Resolver::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        std::string key = task.host + ":" + std::to_string(task.port);
        Result result = std::unexpected(std::string("resolve failed"));
        std::string error_text;
        std::vector<InetAddress> addrs;

        Timestamp resolve_start = nowSteady();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_ADDRCONFIG;
        addrinfo* res = nullptr;
        int rc = ::getaddrinfo(task.host.c_str(), nullptr, &hints, &res);
        if (rc != 0) {
            error_text = std::string("getaddrinfo: ") + ::gai_strerror(rc);
            result = std::unexpected(error_text);
        } else {
            for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                if (p->ai_family == AF_INET) {
                    auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                    InetAddress addr(*sin);
                    addr = InetAddress(addr.toIp(), task.port);
                    addrs.push_back(addr);
                } else if (p->ai_family == AF_INET6) {
                    auto* sin6 = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
                    InetAddress addr(*sin6);
                    addr = InetAddress(addr.toIp(), task.port);
                    addrs.push_back(addr);
                }
            }
            ::freeaddrinfo(res);
            if (addrs.empty()) {
                error_text = "no addresses for " + task.host;
                result = std::unexpected(error_text);
            } else {
                result = addrs;
            }
        }

        SPDLOG_DEBUG("resolver: {} resolved in {} ms", key,
                     std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady() - resolve_start).count());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            CacheEntry entry;
            if (result) {
                entry.addrs = *result;
                entry.expires = nowSteady() + cache_ttl_;
            } else {
                entry.error = result.error();
                entry.expires = nowSteady() + kFailureCacheTtl;
            }
            cache_[key] = std::move(entry);
        }
        postResult(task.loop, std::move(task.cb), std::move(result));
    }
}

}  // namespace gw::net
