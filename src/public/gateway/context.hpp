#pragma once

#include <atomic>
#include <memory>

#include "gateway/config.hpp"

namespace gw::net {
class Resolver;
}  // namespace gw::net

namespace gw::http {
class UpstreamPool;
}  // namespace gw::http

namespace gw::store {
class RedisClient;
}  // namespace gw::store

namespace gw::gateway {

class AccessLog;
class CircuitBreakerSet;
class LedgerWriter;
class Metrics;
class QuotaManager;
class RateLimiter;
class RecentRequests;
class ResponseCache;

// 每个 io loop 的 context() 上挂的服务（one loop per thread，连接不跨线程）
struct IoLoopServices {
    std::shared_ptr<http::UpstreamPool> pool;
    std::shared_ptr<store::RedisClient> redis;  // 未配置 redis 时为空
};

// main 栈上，生命周期覆盖所有 loop。后续任务在这里挂共享服务。
struct GatewayContext {
    std::shared_ptr<const Config> config;
    std::atomic<int> active_sessions{0};
    std::atomic<bool> draining{false};
    net::Resolver* resolver = nullptr;
    Metrics* metrics = nullptr;
    AccessLog* access_log = nullptr;
    RecentRequests* recent = nullptr;
    RateLimiter* rate_limiter = nullptr;
    LedgerWriter* ledger_writer = nullptr;   // 未配置 postgres 时为空
    QuotaManager* quota_manager = nullptr;   // 未配置 redis 时为空
    CircuitBreakerSet* breakers = nullptr;   // 每个上游一个，跨线程共享
    ResponseCache* response_cache = nullptr;  // 响应缓存（16 分片 LRU）
};

}  // namespace gw::gateway
