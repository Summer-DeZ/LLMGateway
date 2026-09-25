#include "gateway/admin.hpp"

#include <nlohmann/json.hpp>

namespace gw::gateway {

RecentRequests::RecentRequests(std::size_t capacity) : capacity_(capacity) {}

void RecentRequests::push(std::shared_ptr<const RequestRecord> record) {
    std::scoped_lock lock(mutex_);
    records_.push_front(std::move(record));
    while (records_.size() > capacity_) records_.pop_back();
}

std::string RecentRequests::renderJson(std::size_t limit) const {
    nlohmann::json arr = nlohmann::json::array();
    std::scoped_lock lock(mutex_);
    std::size_t n = 0;
    for (const auto& record : records_) {
        if (n >= limit) break;
        arr.push_back(record->toJson());
        ++n;
    }
    return arr.dump();
}

std::size_t RecentRequests::size() const {
    std::scoped_lock lock(mutex_);
    return records_.size();
}

bool peerAllowed(const net::TcpConnectionPtr& conn, bool local_only) {
    if (!local_only) return true;
    if (!conn) return false;
    const std::string ip = conn->peerAddress().toIp();
    return ip == "127.0.0.1" || ip == "::1";
}

}  // namespace gw::gateway
