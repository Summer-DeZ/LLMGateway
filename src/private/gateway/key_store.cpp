#include "gateway/key_store.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <stdexcept>

#include "base/log.hpp"

namespace gw::gateway {
namespace {
std::string hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}
std::string metadata(std::string_view alias) {
    const std::string a(alias);
    return "(to_jsonb(" + a +
           ") - 'token_hash' - 'token_last4') || jsonb_build_object('masked_key', 'gwk_…' || " + a +
           ".token_last4)";
}
std::vector<store::PgPreparedStatement> statements() {
    const auto meta = metadata("k");
    return {
        {"keys_load", "SELECT id, token_hash, status FROM api_keys", 0},
        {"keys_conflicts",
         "SELECT key_name FROM usage_daily WHERE key_name LIKE 'managed:%' AND NOT EXISTS (SELECT 1 FROM "
         "api_keys k WHERE key_name = 'managed:' || k.id::text) LIMIT 1",
         0},
        {"keys_create",
         "INSERT INTO api_keys AS k (id,name,token_hash,token_last4,status) VALUES "
         "($1::uuid,$2,$3,$4,'enabled') RETURNING " +
             meta,
         4},
        {"keys_disable",
         "UPDATE api_keys AS k SET status='disabled', disabled_at=coalesce(disabled_at,now()) WHERE "
         "id=$1::uuid RETURNING " +
             meta,
         1},
        {"keys_list",
         "WITH page AS (SELECT * FROM api_keys WHERE ($2::timestamptz IS NULL OR "
         "(created_at,id)<($2::timestamptz,$3::uuid)) ORDER BY created_at DESC,id DESC LIMIT $4::int) "
         "SELECT " +
             meta + R"sql( || jsonb_build_object(
            'today_tokens',coalesce(u.today_tokens,0)::text,'total_tokens',coalesce(u.total_tokens,0)::text,
            'today_unreported_requests',coalesce(u.today_unknown,0)::text,'total_unreported_requests',coalesce(u.total_unknown,0)::text)
            FROM page k LEFT JOIN LATERAL (SELECT sum(billable_tokens) FILTER (WHERE day=$1::date) today_tokens,
            sum(billable_tokens) total_tokens, sum(unreported_requests) FILTER (WHERE day=$1::date) today_unknown,
            sum(unreported_requests) total_unknown FROM usage_daily WHERE key_name='managed:' || k.id::text) u ON true
            ORDER BY k.created_at DESC,k.id DESC)sql",
         4},
        {"keys_get", "SELECT " + meta + " FROM api_keys k WHERE id=$1::uuid", 1},
        {"keys_usage", R"sql(SELECT jsonb_build_object('day',day,'requests',coalesce(sum(requests),0)::text,
            'input_tokens',coalesce(sum(input_tokens),0)::text,'output_tokens',coalesce(sum(output_tokens),0)::text,
            'cached_tokens',coalesce(sum(cached_tokens),0)::text,'billable_tokens',coalesce(sum(billable_tokens),0)::text,
            'unreported_requests',coalesce(sum(unreported_requests),0)::text,'truncated_requests',coalesce(sum(truncated_requests),0)::text)
            FROM usage_daily WHERE key_name='managed:' || $1::uuid::text AND day BETWEEN $2::date AND $3::date GROUP BY ROLLUP(day) ORDER BY day)sql",
         3},
        {"keys_requests",
         R"sql(SELECT jsonb_build_object('ledger_id',ledger_id,'finished_at',finished_at,'model',model,
            'outcome',outcome,'http_status',http_status,'input_tokens',input_tokens::text,'output_tokens',output_tokens::text,
            'cached_tokens',cached_tokens::text,'billable_tokens',billable_tokens::text,'usage_complete',usage_complete)
            FROM usage_records WHERE key_name='managed:' || $1::uuid::text AND billing_day BETWEEN $2::date AND $3::date
            AND ($4::timestamptz IS NULL OR (finished_at,ledger_id)<($4::timestamptz,$5))
            ORDER BY finished_at DESC,ledger_id DESC LIMIT $6::int)sql",
         6}};
}
store::PgError unavailable() { return {"key storage unavailable", "", true}; }
}  // namespace

std::string keyDigest(std::string_view secret) {
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    if (EVP_Digest(secret.data(), secret.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != 32)
        throw std::runtime_error("key digest failed");
    return hex(digest.data(), digest.size());
}
std::string generateApiKey() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("secure random unavailable");
    return "gwk_" + hex(bytes.data(), bytes.size());
}
std::string generateKeyId() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("secure random unavailable");
    bytes[6] = static_cast<unsigned char>((bytes[6] & 15) | 64);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 63) | 128);
    auto s = hex(bytes.data(), bytes.size());
    return s.substr(0, 8) + "-" + s.substr(8, 4) + "-" + s.substr(12, 4) + "-" + s.substr(16, 4) + "-" +
           s.substr(20);
}
KeyIdentity KeyRegistry::find(std::string_view secret) const {
    auto snapshot = snapshot_.load();
    auto it = snapshot->find(keyDigest(secret));
    return it == snapshot->end() ? KeyIdentity{} : it->second;
}
void KeyRegistry::publish(std::shared_ptr<const Snapshot> snapshot) { snapshot_.store(std::move(snapshot)); }

KeyStore::KeyStore(net::EventLoop* loop, const Config& config, KeyRegistry& registry)
    : loop_(loop),
      registry_(registry),
      pg_(std::make_shared<store::PgClient>(loop, config.postgres->dsn, config.postgres->connect_timeout)) {
    for (const auto& [key, user] : config.users) static_hashes_.insert(keyDigest(key));
}
KeyStore::~KeyStore() {
    *alive_ = false;
    if (recovery_timer_.valid()) loop_->cancel(recovery_timer_);
}
bool KeyStore::staticConflict(std::string_view secret) const {
    return static_hashes_.contains(keyDigest(secret));
}
void KeyStore::start() {
    busy_ = true;
    pg_->connect([this](auto result) {
        if (!result) {
            busy_ = false;
            return;
        }
        busy_ = false;
        recover();
    });
    recovery_timer_ = loop_->runEvery(std::chrono::seconds(1), [this] {
        if (!pg_->connected()) registry_.ready = false;
        if (!registry_.ready && !busy_) recover();
    });
}
void KeyStore::reload(std::function<void(bool)> done) {
    pg_->execute("keys_conflicts", {}, [this, done](auto conflicts) {
        if (!conflicts || !conflicts->rows.empty()) {
            registry_.ready = false;
            done(false);
            return;
        }
        pg_->execute("keys_load", {}, [this, done](auto result) {
            if (!result) {
                registry_.ready = false;
                done(false);
                return;
            }
            auto snapshot = std::make_shared<KeyRegistry::Snapshot>();
            for (const auto& row : result->rows) {
                const auto& hash = *row.columns[1];
                if (static_hashes_.contains(hash)) {
                    registry_.ready = false;
                    done(false);
                    return;
                }
                auto user = std::make_shared<UserConfig>();
                user->name = "managed:" + *row.columns[0];
                user->key = user->name;  // No raw credential in a principal or rate ticket.
                snapshot->emplace(hash, KeyIdentity{std::move(user), row.columns[2] == "enabled"});
            }
            registry_.publish(std::move(snapshot));
            registry_.ready = true;
            done(true);
        });
    });
}
void KeyStore::stop() {
    loop_->assertInLoopThread();
    stopping_ = true;
    registry_.ready = false;
    if (recovery_timer_.valid()) {
        loop_->cancel(recovery_timer_);
        recovery_timer_ = {};
    }
}
void KeyStore::recover() {
    if (stopping_ || !pg_->idle() || busy_) return;
    if (!prepared_) {
        busy_ = true;
        pg_->prepare(statements(), [this](auto result) {
            busy_ = false;
            prepared_ = result.has_value();
            if (prepared_) recover();
        });
        return;
    }
    busy_ = true;
    pg_->rawBatch({"SET statement_timeout = '5s'", "SET TIME ZONE 'UTC'"}, [this](auto set) {
        if (!set) {
            busy_ = false;
            return;
        }
        reload([this](bool) {
            busy_ = false;
            next();
        });
    });
}
void KeyStore::execute(std::string statement, store::PgParams params, bool mutation,
                       store::PgQueryCallback done) {
    loop_->assertInLoopThread();
    if (stopping_ || !registry_.ready || commands_.size() >= 64 || !pg_->connected()) {
        done(std::unexpected(unavailable()));
        return;
    }
    commands_.push_back({std::move(statement), std::move(params), mutation, std::move(done)});
    next();
}
void KeyStore::next() {
    if (busy_ || commands_.empty()) return;
    busy_ = true;
    if (stopping_ || !registry_.ready) {
        finish(std::unexpected(unavailable()));
        return;
    }
    const auto& cmd = commands_.front();
    pg_->execute(cmd.statement, cmd.params, [this](auto result) {
        if (!result) {
            // A commit acknowledgement may have been lost. Pause until a full reload.
            if (commands_.front().mutation || result.error().connection_lost) registry_.ready = false;
            finish(std::unexpected(unavailable()));
        } else if (commands_.front().mutation) {
            reload([this, result = std::move(result)](bool ok) mutable {
                finish(ok ? std::move(result)
                          : std::expected<store::PgResult, store::PgError>(std::unexpected(unavailable())));
            });
        } else
            finish(std::move(result));
    });
}
void KeyStore::finish(std::expected<store::PgResult, store::PgError> result) {
    auto done = std::move(commands_.front().done);
    commands_.pop_front();
    busy_ = false;
    done(std::move(result));
    loop_->queueInLoop([this, alive = alive_] { if (*alive) next(); });
}
}  // namespace gw::gateway
