#include "gateway/response_cache.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstring>

namespace gw::gateway {

namespace {

std::string keyToString(const std::vector<std::uint8_t>& key) {
    return std::string(reinterpret_cast<const char*>(key.data()), key.size());
}

}  // namespace

std::vector<std::uint8_t> ResponseCache::makeKey(const std::string& protocol, bool stream, bool client_wants_usage,
                                                 const std::string& model_alias, const nlohmann::json& body) {
    // 规范化：删除 user、metadata、stream、stream_options，再 dump（nlohmann 对象键有序 → 规范形式）
    nlohmann::json normalized = body;
    if (normalized.is_object()) {
        normalized.erase("user");
        normalized.erase("metadata");
        normalized.erase("stream");
        normalized.erase("stream_options");
    }
    std::string material = "v1|" + protocol + "|" + (stream ? "s" : "n") + "|" +
                           (client_wants_usage ? "u" : "-") + "|" + model_alias + "|" + normalized.dump();

    std::vector<std::uint8_t> digest(32);
    unsigned int len = 0;
    EVP_Digest(material.data(), material.size(), digest.data(), &len, EVP_sha256(), nullptr);
    digest.resize(len);
    return digest;
}

ResponseCache::ResponseCache(const CacheConfig& config) : enabled_(config.enabled), config_(config) {
    budget_per_shard_ = config_.max_bytes / kShards;
}

ResponseCache::~ResponseCache() = default;

ResponseCache::Shard& ResponseCache::shardFor(const std::vector<std::uint8_t>& key) {
    // 用摘要首字节分片（键均匀分布）
    std::size_t index = key.empty() ? 0 : key[0] % kShards;
    return shards_[index];
}

const ResponseCache::Shard& ResponseCache::shardFor(const std::vector<std::uint8_t>& key) const {
    std::size_t index = key.empty() ? 0 : key[0] % kShards;
    return shards_[index];
}

std::optional<ResponseCache::EntryPtr> ResponseCache::get(const std::vector<std::uint8_t>& key) {
    if (!enabled_) return std::nullopt;
    Shard& shard = shardFor(key);
    std::scoped_lock lock(mutexes_[&shard - &shards_[0]]);
    auto it = shard.map.find(keyToString(key));
    if (it == shard.map.end()) return std::nullopt;
    EntryPtr entry = it->second->second;
    if (nowSteady() > entry->expires_at) {
        // 惰性过期
        shard.bytes -= entry->body.size() + entry->key.size();
        shard.lru.erase(it->second);
        shard.map.erase(it);
        return std::nullopt;
    }
    // 移到表头（最近使用在前）
    shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
    return entry;
}

void ResponseCache::put(std::vector<std::uint8_t> key, std::shared_ptr<Entry> entry) {
    if (!enabled_ || key.empty()) return;
    if (entry->body.size() > config_.max_entry_bytes) return;  // 单条超限不写入
    Shard& shard = shardFor(key);
    std::scoped_lock lock(mutexes_[&shard - &shards_[0]]);
    std::string key_str = keyToString(key);
    auto it = shard.map.find(key_str);
    if (it != shard.map.end()) {
        shard.bytes -= it->second->second->body.size() + it->second->second->key.size();
        shard.lru.erase(it->second);
        shard.map.erase(it);
    }
    entry->expires_at = nowSteady() + std::chrono::seconds(config_.ttl_seconds);
    shard.lru.push_front({std::move(key), entry});
    shard.map[key_str] = shard.lru.begin();
    shard.bytes += entry->body.size() + entry->key.size();
    // 从表尾淘汰直到不超预算
    while (shard.bytes > budget_per_shard_ && !shard.lru.empty()) {
        auto& victim = shard.lru.back();
        shard.bytes -= victim.second->body.size() + victim.second->key.size();
        shard.map.erase(keyToString(victim.first));
        shard.lru.pop_back();
    }
}

void ResponseCache::sweep() {
    if (!enabled_) return;
    Timestamp now = nowSteady();
    for (std::size_t i = 0; i < kShards; ++i) {
        Shard& shard = shards_[i];
        std::scoped_lock lock(mutexes_[i]);
        for (auto it = shard.lru.begin(); it != shard.lru.end();) {
            if (now > it->second->expires_at) {
                shard.bytes -= it->second->body.size() + it->second->key.size();
                shard.map.erase(keyToString(it->first));
                it = shard.lru.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::pair<std::uint64_t, std::uint64_t> ResponseCache::stats() const {
    std::uint64_t bytes = 0;
    std::uint64_t entries = 0;
    for (std::size_t i = 0; i < kShards; ++i) {
        std::scoped_lock lock(mutexes_[i]);
        bytes += shards_[i].bytes;
        entries += shards_[i].lru.size();
    }
    return {bytes, entries};
}

}  // namespace gw::gateway
