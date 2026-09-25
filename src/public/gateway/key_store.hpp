#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "gateway/config.hpp"
#include "store/pg_client.hpp"

namespace gw::gateway {

std::string keyDigest(std::string_view secret);
std::string generateApiKey();
std::string generateKeyId();

struct KeyIdentity {
    std::shared_ptr<const UserConfig> user;
    bool enabled = false;
};

// Published once per committed mutation; readers keep their principal alive.
class KeyRegistry {
   public:
    using Snapshot = std::unordered_map<std::string, KeyIdentity>;
    std::atomic<bool> ready{false};
    KeyIdentity find(std::string_view secret) const;
    void publish(std::shared_ptr<const Snapshot> snapshot);

   private:
    std::atomic<std::shared_ptr<const Snapshot>> snapshot_{std::make_shared<const Snapshot>()};
};

// All methods except registry access run on the ledger loop. Commands are serialized.
class KeyStore {
   public:
    KeyStore(net::EventLoop* loop, const Config& config, KeyRegistry& registry);
    ~KeyStore();
    void start();
    void stop();
    void execute(std::string statement, store::PgParams params, bool mutation, store::PgQueryCallback done);
    bool staticConflict(std::string_view secret) const;

   private:
    struct Command {
        std::string statement;
        store::PgParams params;
        bool mutation;
        store::PgQueryCallback done;
    };
    void reload(std::function<void(bool)> done);
    void recover();
    void next();
    void finish(std::expected<store::PgResult, store::PgError> result);
    net::EventLoop* loop_;
    KeyRegistry& registry_;
    std::shared_ptr<store::PgClient> pg_;
    std::unordered_set<std::string> static_hashes_;
    std::deque<Command> commands_;
    bool busy_ = false;
    bool stopping_ = false;
    bool prepared_ = false;
    net::TimerId recovery_timer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace gw::gateway
