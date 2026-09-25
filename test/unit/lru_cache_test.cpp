#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/response_cache.hpp"

namespace {

using gw::gateway::CacheConfig;
using gw::gateway::ResponseCache;

CacheConfig testConfig(std::size_t max_bytes = 16 * 100, std::size_t max_entry = 100, int ttl = 10) {
    CacheConfig cfg;
    cfg.enabled = true;
    cfg.max_bytes = max_bytes;
    cfg.max_entry_bytes = max_entry;
    cfg.ttl_seconds = ttl;
    return cfg;
}

std::shared_ptr<ResponseCache::Entry> makeEntry(std::string body, std::string upstream = "qwen36") {
    auto entry = std::make_shared<ResponseCache::Entry>();
    entry->body = std::move(body);
    entry->usage.prompt = 10;
    entry->usage.cached = 2;
    entry->usage.prompt_computed = 8;
    entry->usage.completion = 5;
    entry->usage.source = gw::gateway::AttemptUsage::Source::upstream_final;
    entry->upstream = std::move(upstream);
    return entry;
}

nlohmann::json req(std::string content = "hello", std::optional<double> temp = 0.0) {
    nlohmann::json j;
    j["model"] = "qwen";
    j["messages"] = nlohmann::json::array({nlohmann::json{{"role", "user"}, {"content", content}}});
    if (temp) j["temperature"] = *temp;
    return j;
}

// 找若干个落在同一分片的键（分片 = key[0] % kShards，测试里用 key[0] 直接构造）
std::vector<std::vector<std::uint8_t>> sameShardKeys(std::size_t count) {
    std::vector<std::vector<std::uint8_t>> keys;
    std::uint8_t target = 0;
    bool target_set = false;
    for (int i = 0; i < 10000 && keys.size() < count; ++i) {
        auto key = ResponseCache::makeKey("openai", false, false, "qwen", req("content-" + std::to_string(i)));
        if (!target_set) {
            target = key[0];
            target_set = true;
        }
        if (key[0] == target) keys.push_back(key);
    }
    return keys;
}

TEST(LruCacheTest, MissThenHit) {
    ResponseCache cache(testConfig());
    auto key = ResponseCache::makeKey("openai", false, false, "qwen", req());
    EXPECT_FALSE(cache.get(key).has_value());
    auto entry = makeEntry("{\"ok\":true}");
    entry->key = key;
    cache.put(key, entry);
    auto hit = cache.get(key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ((*hit)->body, "{\"ok\":true}");
    EXPECT_EQ((*hit)->upstream, "qwen36");
}

TEST(LruCacheTest, LruEvictionOrderAndByteBudget) {
    ResponseCache cache(testConfig(/*max_bytes=*/16 * 100, /*max_entry=*/100));
    auto keys = sameShardKeys(3);
    ASSERT_EQ(keys.size(), 3u);
    // 每条约 40 字节 + 32 字节键 = 72；分片预算 100，只能放 1 条
    for (std::size_t i = 0; i < 3; ++i) {
        auto entry = makeEntry(std::string(40, 'a' + static_cast<char>(i)));
        entry->key = keys[i];
        cache.put(keys[i], entry);
    }
    // 最新写入的还在；最早的被淘汰
    EXPECT_TRUE(cache.get(keys[2]).has_value());
    EXPECT_FALSE(cache.get(keys[0]).has_value());
    auto [bytes, entries] = cache.stats();
    (void)bytes;
    EXPECT_LE(entries, 2u);

    // LRU：命中 keys[1] 使其前移，再插入一条 → 被淘汰的是 keys[2]（原表尾）
    // 先清空预算内的顺序：当前分片内应有 keys[1] 与 keys[2]
    // 重新构建：只用 2 个同分片键验证访问顺序
    ResponseCache cache2(testConfig(16 * 100, 100));
    auto k2 = sameShardKeys(2);
    ASSERT_EQ(k2.size(), 2u);
    auto e0 = makeEntry(std::string(40, 'x'));
    e0->key = k2[0];
    cache2.put(k2[0], e0);
    auto e1 = makeEntry(std::string(40, 'y'));
    e1->key = k2[1];
    cache2.put(k2[1], e1);  // 预算 100，两条 72+72=144 → 淘汰 k2[0]
    EXPECT_FALSE(cache2.get(k2[0]).has_value());
    EXPECT_TRUE(cache2.get(k2[1]).has_value());
}

TEST(LruCacheTest, LazyExpiryAndSweep) {
    ResponseCache cache(testConfig(16 * 1000, 1000, /*ttl=*/0));
    auto key = ResponseCache::makeKey("openai", false, false, "qwen", req());
    auto entry = makeEntry("body");
    entry->key = key;
    cache.put(key, entry);
    // ttl=0：立即过期（惰性删除）
    EXPECT_FALSE(cache.get(key).has_value());
    auto [bytes, entries] = cache.stats();
    (void)bytes;
    EXPECT_EQ(entries, 0u);

    // sweep 清理：ttl=0 的条目在 sweep 后消失
    ResponseCache cache2(testConfig(16 * 1000, 1000, 0));
    auto key2 = ResponseCache::makeKey("openai", false, false, "qwen", req());
    auto entry2 = makeEntry("body");
    entry2->key = key2;
    cache2.put(key2, entry2);
    cache2.sweep();
    auto [b2, e2] = cache2.stats();
    (void)b2;
    EXPECT_EQ(e2, 0u);
}

TEST(LruCacheTest, SingleEntryOverLimitNotWritten) {
    ResponseCache cache(testConfig(16 * 1000, /*max_entry=*/10, 10));
    auto key = ResponseCache::makeKey("openai", false, false, "qwen", req());
    auto entry = makeEntry(std::string(11, 'z'));
    entry->key = key;
    cache.put(key, entry);
    EXPECT_FALSE(cache.get(key).has_value());
}

TEST(LruCacheTest, LastEntryCannotExceedShardBudget) {
    ResponseCache cache(testConfig(1600, 1000));
    auto key = ResponseCache::makeKey("openai", false, false, "qwen", req());
    auto entry = makeEntry(std::string(1000, 'x'));
    entry->key = key;
    cache.put(key, entry);
    EXPECT_FALSE(cache.get(key));
    EXPECT_EQ(cache.stats().first, 0u);
}

TEST(LruCacheTest, KeyNormalizationFieldOrder) {
    nlohmann::json a;
    a["model"] = "qwen";
    a["temperature"] = 0.0;
    a["messages"] = nlohmann::json::array({nlohmann::json{{"role", "user"}, {"content", "hi"}}});
    a["max_tokens"] = 16;
    nlohmann::json b;
    b["max_tokens"] = 16;
    b["messages"] = nlohmann::json::array({nlohmann::json{{"role", "user"}, {"content", "hi"}}});
    b["temperature"] = 0.0;
    b["model"] = "qwen";
    EXPECT_EQ(ResponseCache::makeKey("openai", false, false, "qwen", a),
              ResponseCache::makeKey("openai", false, false, "qwen", b));
}

TEST(LruCacheTest, KeyUserFieldIgnored) {
    auto a = req();
    a["user"] = "alice";
    auto b = req();
    b["user"] = "bob";
    EXPECT_EQ(ResponseCache::makeKey("openai", false, false, "qwen", a),
              ResponseCache::makeKey("openai", false, false, "qwen", b));
}

TEST(LruCacheTest, KeyStreamAndProtocolAndUsageAffectKey) {
    auto base = req();
    EXPECT_NE(ResponseCache::makeKey("openai", false, false, "qwen", base),
              ResponseCache::makeKey("openai", true, false, "qwen", base));
    EXPECT_NE(ResponseCache::makeKey("openai", false, false, "qwen", base),
              ResponseCache::makeKey("anthropic", false, false, "qwen", base));
    EXPECT_NE(ResponseCache::makeKey("openai", false, false, "qwen", base),
              ResponseCache::makeKey("openai", false, true, "qwen", base));
    EXPECT_NE(ResponseCache::makeKey("openai", false, false, "qwen", base),
              ResponseCache::makeKey("openai", false, false, "qwen-anth", base));
}

TEST(LruCacheTest, KeyStreamOptionsIgnored) {
    auto a = req();
    a["stream_options"] = {{"include_usage", true}};
    a["stream"] = true;
    auto b = req();
    // 规范化删除 stream/stream_options → 同一键（stream 维度单独参与）
    EXPECT_EQ(ResponseCache::makeKey("openai", true, true, "qwen", a),
              ResponseCache::makeKey("openai", true, true, "qwen", b));
}

}  // namespace
