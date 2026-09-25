#include "gateway/rate_limiter.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace gw::gateway {

namespace {

std::unique_ptr<TokenBucket> makeBucket(const Clock& clock, double limit, double burst) {
    if (limit <= 0) return nullptr;  // 0 或缺省 = 该维度不限
    double per_sec = limit / 60.0;               // 补充速率：限额 / 60 每秒（policies.md §1.1）
    double capacity = burst > 0 ? burst : limit;  // 容量（突发）= 限额（或显式 burst）
    return std::make_unique<TokenBucket>(clock, capacity, per_sec);
}

// 需要操作的桶列表（存在的才参与）；顺序固定：user.rpm, user.tpm, model.rpm, model.tpm
void collect(TokenBucket* a, TokenBucket* b, TokenBucket* c, TokenBucket* d, std::vector<TokenBucket*>& out) {
    for (TokenBucket* bucket : {a, b, c, d}) {
        if (bucket != nullptr) out.push_back(bucket);
    }
}

}  // namespace

RateLimiter::RateLimiter(const Config& config, const Clock& clock) : clock_(&clock) {
    for (const auto& [key, user] : config.users) {
        BucketPair pair;
        pair.rpm = makeBucket(clock, user.rpm, user.rpm_burst.value_or(0));
        pair.tpm = makeBucket(clock, user.tpm, user.tpm_burst.value_or(0));
        user_buckets_.emplace(key, std::move(pair));
    }
    for (const auto& [alias, model] : config.models) {
        BucketPair pair;
        pair.rpm = makeBucket(clock, model.rpm, model.rpm_burst.value_or(0));
        pair.tpm = makeBucket(clock, model.tpm, model.tpm_burst.value_or(0));
        model_buckets_.emplace(alias, std::move(pair));
    }
}

std::expected<std::shared_ptr<RateTicket>, RateRejection> RateLimiter::tryAcquire(const UserConfig& user,
                                                                                  const ModelConfig& model,
                                                                                  double reserve_tokens) {
    auto uit = user_buckets_.find(user.key);
    auto mit = model_buckets_.find(model.alias);
    BucketPair* ub = uit == user_buckets_.end() ? nullptr : &uit->second;
    BucketPair* mb = mit == model_buckets_.end() ? nullptr : &mit->second;

    std::vector<TokenBucket*> involved;
    collect(ub ? ub->rpm.get() : nullptr, ub ? ub->tpm.get() : nullptr, mb ? mb->rpm.get() : nullptr,
            mb ? mb->tpm.get() : nullptr, involved);

    // 桶总是按固定层级顺序加锁（user → model、rpm → tpm），满足全局锁序，不会死锁
    if (!involved.empty()) {
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(involved.size());
        for (auto* b : involved) locks.emplace_back(b->mutex());

        Timestamp now = clock_->now();
        for (auto* b : involved) b->refill(now);

        // 全部 canTake 才继续；任一不足整体拒绝且不扣任何桶
        TokenBucket* reject_bucket = nullptr;
        double reject_need = 0;
        const char* reject_scope = nullptr;
        const char* reject_dimension = nullptr;
        auto check = [&](TokenBucket* bucket, double need, const char* scope, const char* dimension) {
            if (bucket == nullptr) return;
            if (!bucket->canTake(need)) {
                if (reject_bucket == nullptr || bucket->waitFor(need) > reject_bucket->waitFor(reject_need)) {
                    reject_bucket = bucket;
                    reject_need = need;
                    reject_scope = scope;
                    reject_dimension = dimension;
                }
            }
        };
        check(ub ? ub->rpm.get() : nullptr, 1.0, "user", "requests");
        check(ub ? ub->tpm.get() : nullptr, reserve_tokens, "user", "tokens");
        check(mb ? mb->rpm.get() : nullptr, 1.0, "model", "requests");
        check(mb ? mb->tpm.get() : nullptr, reserve_tokens, "model", "tokens");
        if (reject_bucket != nullptr) {
            return std::unexpected(
                RateRejection{reject_scope, reject_dimension, reject_bucket->waitFor(reject_need)});
        }

        auto ticket = std::make_shared<RateTicket>();
        ticket->user_key = user.key;
        ticket->model_alias = model.alias;
        ticket->reserved_tokens = reserve_tokens;
        auto takeAll = [&](TokenBucket* bucket, double n) {
            if (bucket == nullptr) return;
            bucket->take(n);
        };
        takeAll(ub ? ub->rpm.get() : nullptr, 1.0);
        takeAll(ub ? ub->tpm.get() : nullptr, reserve_tokens);
        takeAll(mb ? mb->rpm.get() : nullptr, 1.0);
        takeAll(mb ? mb->tpm.get() : nullptr, reserve_tokens);

        // 快照：用户与模型两个维度中较紧的值（余量取扣减后的当前值）
        auto tighterLimit = [](std::optional<double> a, std::optional<double> b) -> std::optional<double> {
            if (!a) return b;
            if (!b) return a;
            return std::min(*a, *b);
        };
        auto tighterRemaining = [](std::optional<double> a, std::optional<double> b) -> std::optional<double> {
            if (!a) return b;
            if (!b) return a;
            return std::min(*a, *b);
        };
        std::optional<double> limit_r, limit_t, rem_r, rem_t;
        if (ub && ub->rpm) {
            limit_r = ub->rpm->capacity();
            rem_r = std::max(0.0, ub->rpm->tokens());
        }
        if (mb && mb->rpm) {
            limit_r = tighterLimit(limit_r, mb->rpm->capacity());
            rem_r = tighterRemaining(rem_r, std::max(0.0, mb->rpm->tokens()));
        }
        if (ub && ub->tpm) {
            limit_t = ub->tpm->capacity();
            rem_t = std::max(0.0, ub->tpm->tokens());
        }
        if (mb && mb->tpm) {
            limit_t = tighterLimit(limit_t, mb->tpm->capacity());
            rem_t = tighterRemaining(rem_t, std::max(0.0, mb->tpm->tokens()));
        }
        ticket->limit_requests = limit_r;
        ticket->remaining_requests = rem_r;
        ticket->limit_tokens = limit_t;
        ticket->remaining_tokens = rem_t;
        return ticket;
    }

    // 没有任何桶（用户与模型都未配置限额）：直接放行
    auto ticket = std::make_shared<RateTicket>();
    ticket->user_key = user.key;
    ticket->model_alias = model.alias;
    ticket->reserved_tokens = reserve_tokens;
    return ticket;
}

void RateLimiter::settle(const RateTicket& ticket, std::optional<std::int64_t> actual) {
    if (!actual.has_value()) return;  // 有字段未报告：该部分不退还预留（保守处理）
    double refund = ticket.reserved_tokens - static_cast<double>(*actual);
    if (refund == 0) return;

    std::vector<TokenBucket*> involved;
    auto uit = user_buckets_.find(ticket.user_key);
    auto mit = model_buckets_.find(ticket.model_alias);
    collect(uit == user_buckets_.end() ? nullptr : uit->second.tpm.get(),
            mit == model_buckets_.end() ? nullptr : mit->second.tpm.get(), nullptr, nullptr, involved);
    if (involved.empty()) return;

    // 与 tryAcquire 相同的固定锁序
    std::vector<std::unique_lock<std::mutex>> locks;
    for (auto* b : involved) locks.emplace_back(b->mutex());
    Timestamp now = clock_->now();
    for (auto* b : involved) {
        b->refill(now);
        b->adjust(refund);  // 正数退还、负数补扣，下限 -capacity
    }
}

std::optional<double> RateLimiter::userTokens(const std::string& user_key) const {
    auto it = user_buckets_.find(user_key);
    if (it == user_buckets_.end() || !it->second.tpm) return std::nullopt;
    std::scoped_lock lock(it->second.tpm->mutex());
    return it->second.tpm->tokens();
}

std::optional<double> RateLimiter::modelTokens(const std::string& model_alias) const {
    auto it = model_buckets_.find(model_alias);
    if (it == model_buckets_.end() || !it->second.tpm) return std::nullopt;
    std::scoped_lock lock(it->second.tpm->mutex());
    return it->second.tpm->tokens();
}

}  // namespace gw::gateway
