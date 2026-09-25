#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "gateway/config.hpp"
#include "gateway/usage_meter.hpp"

namespace gw::gateway {

// 响应缓存（policies.md §2）：16 分片，每片 LRU 列表 + 键映射 + 一把 mutex + 字节预算。
// 键是 SHA-256("v1|入口协议|s/n|u/-|模型别名|规范化请求体")。不做 single-flight。
class ResponseCache : noncopyable {
public:
    struct Entry {
        std::vector<std::uint8_t> key;  // 32 字节
        std::string body;               // 流式：规范化 SSE 帧（含 [DONE]）；非流式：JSON 体
        bool stream = false;
        std::string content_type;
        AttemptUsage usage;  // 原始上游 usage（命中时使用）
        std::string upstream;
        std::string finish_reason;
        Timestamp expires_at;
    };
    using EntryPtr = std::shared_ptr<const Entry>;

    explicit ResponseCache(const CacheConfig& config);
    ~ResponseCache();

    bool enabled() const { return enabled_; }
    const CacheConfig& config() const { return config_; }
    std::size_t maxEntryBytes() const { return config_.max_entry_bytes; }

    // 键：入口协议、stream、客户端是否要 usage、模型别名、规范化请求体
    static std::vector<std::uint8_t> makeKey(const std::string& protocol, bool stream, bool client_wants_usage,
                                             const std::string& model_alias, const nlohmann::json& body);

    std::optional<EntryPtr> get(const std::vector<std::uint8_t>& key);
    void put(std::vector<std::uint8_t> key, std::shared_ptr<Entry> entry);
    // main loop 每 30 秒调用：删除各分片过期项
    void sweep();
    // 指标：{总字节, 总条目}
    std::pair<std::uint64_t, std::uint64_t> stats() const;

private:
    struct Shard {
        std::list<std::pair<std::vector<std::uint8_t>, EntryPtr>> lru;  // 最近使用在前
        std::unordered_map<std::string, std::list<std::pair<std::vector<std::uint8_t>, EntryPtr>>::iterator> map;
        std::uint64_t bytes = 0;
    };

    static constexpr std::size_t kShards = 16;

    Shard& shardFor(const std::vector<std::uint8_t>& key);
    const Shard& shardFor(const std::vector<std::uint8_t>& key) const;

    bool enabled_ = true;
    CacheConfig config_;
    mutable std::array<std::mutex, kShards> mutexes_;
    std::array<Shard, kShards> shards_;
    std::uint64_t budget_per_shard_ = 0;
};

}  // namespace gw::gateway
