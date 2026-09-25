#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "gateway/config.hpp"
#include "gateway/token_bucket.hpp"

namespace gw::gateway {

// 一次限流预留。预留不是消耗：结束时按上游报告的真实值结算（policies.md §1.3）
struct RateTicket {
    std::string user_key;
    std::string model_alias;
    double reserved_tokens = 0;
    // 通过时响应头用的快照：用户与模型两个维度中较紧的值；nullopt 表示该维度不限
    std::optional<double> limit_requests;
    std::optional<double> remaining_requests;
    std::optional<double> limit_tokens;
    std::optional<double> remaining_tokens;
};

struct RateRejection {
    std::string scope;      // user | model
    std::string dimension;  // requests | tokens
    Duration retry_after{};
};

// 4 个令牌桶（user.rpm / user.tpm / model.rpm / model.tpm），配置为 0 或缺省表示该维度不限。
// 桶在启动时按配置预建，运行期只读查找；准入用 std::lock 同时锁住涉及的桶，任一不足整体拒绝且不扣任何桶
class RateLimiter : noncopyable {
public:
    RateLimiter(const Config& config, const Clock& clock);

    std::expected<std::shared_ptr<RateTicket>, RateRejection> tryAcquire(const UserConfig& user,
                                                                         const ModelConfig& model,
                                                                         double reserve_tokens);
    // 结算：两个 tpm 桶 adjust(reserved - actual)。actual 未知（nullopt）时不退还预留（保守）
    void settle(const RateTicket& ticket, std::optional<std::int64_t> actual);

    // 桶余额查看（/admin 与单测用）
    std::optional<double> userTokens(const std::string& user_key) const;
    std::optional<double> modelTokens(const std::string& model_alias) const;

private:
    struct BucketPair {
        std::unique_ptr<TokenBucket> rpm;  // 任一可为 null（不限）
        std::unique_ptr<TokenBucket> tpm;
    };

    std::unordered_map<std::string, BucketPair> user_buckets_;  // key: users.*.key
    std::unordered_map<std::string, BucketPair> model_buckets_;  // key: 模型别名
    const Clock* clock_;
};

}  // namespace gw::gateway
