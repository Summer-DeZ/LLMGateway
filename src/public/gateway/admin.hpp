#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "base/noncopyable.hpp"
#include "gateway/request_record.hpp"
#include "net/tcp_connection.hpp"

namespace gw::gateway {

// 最近请求的环形缓冲（/admin/requests，policies.md §3.4），新的在前
class RecentRequests : noncopyable {
public:
    explicit RecentRequests(std::size_t capacity = 256);

    void push(std::shared_ptr<const RequestRecord> record);
    // JSON 数组字符串，最多 limit 条
    std::string renderJson(std::size_t limit) const;
    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::deque<std::shared_ptr<const RequestRecord>> records_;
};

// admin.listen_local_only 时 /metrics 与 /admin/* 只允许本机来源
bool peerAllowed(const net::TcpConnectionPtr& conn, bool local_only);

}  // namespace gw::gateway
