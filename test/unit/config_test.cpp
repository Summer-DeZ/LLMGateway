#include "gateway/config.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace {

class TempFile {
public:
    explicit TempFile(const std::string& content) {
        path_ = "/tmp/gw_config_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++);
        std::ofstream out(path_);
        out << content;
    }
    ~TempFile() { ::unlink(path_.c_str()); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    static int counter_;
};

int TempFile::counter_ = 0;

}  // namespace

TEST(ConfigTest, LoadsBasicConfig) {
    TempFile file(R"({
        "server": { "listen": "127.0.0.1:18080", "io_threads": 2 },
        "users": { "gk-a": { "name": "a", "rpm": 10 } },
        "upstreams": {
            "up1": { "kind": "openai-chat", "base_url": "http://127.0.0.1:10010/v1", "model": "m" }
        },
        "models": { "qwen": { "targets": ["up1"], "rpm": 5 } }
    })");
    auto cfg = gw::gateway::Config::load(file.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error();
    EXPECT_EQ((*cfg)->server.listen, "127.0.0.1:18080");
    EXPECT_EQ((*cfg)->server.io_threads, 2);
    ASSERT_EQ((*cfg)->users.size(), 1u);
    EXPECT_EQ((*cfg)->users.at("gk-a").name, "a");
    ASSERT_EQ((*cfg)->models.size(), 1u);
    EXPECT_EQ((*cfg)->models.at("qwen").targets.at(0), "up1");
    ASSERT_TRUE((*cfg)->findUpstream("up1") != nullptr);
    EXPECT_EQ((*cfg)->findUpstream("up1")->url.port, 10010);
}

TEST(ConfigTest, EnvSubstitution) {
    ::setenv("GW_TEST_DSN", "host=127.0.0.1 port=5433", 1);
    TempFile file(R"({
        "postgres": { "dsn": "env:GW_TEST_DSN" },
        "upstreams": {
            "up1": { "kind": "openai-chat", "base_url": "http://127.0.0.1:10010/v1", "model": "m",
                     "api_key": "env:GW_TEST_MISSING_KEY_XYZ" }
        },
        "models": { "qwen": { "targets": ["up1"] } }
    })");
    auto cfg = gw::gateway::Config::load(file.path());
    ASSERT_FALSE(cfg.has_value());
    EXPECT_NE(cfg.error().find("GW_TEST_MISSING_KEY_XYZ"), std::string::npos);
}

TEST(ConfigTest, EnvSubstitutionSuccess) {
    ::setenv("GW_TEST_DSN", "host=127.0.0.1 port=5433", 1);
    TempFile file(R"({
        "postgres": { "dsn": "env:GW_TEST_DSN" },
        "upstreams": {
            "up1": { "kind": "openai-chat", "base_url": "http://127.0.0.1:10010/v1", "model": "m" }
        },
        "models": { "qwen": { "targets": ["up1"] } }
    })");
    auto cfg = gw::gateway::Config::load(file.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error();
    ASSERT_TRUE((*cfg)->postgres.has_value());
    EXPECT_EQ((*cfg)->postgres->dsn, "host=127.0.0.1 port=5433");
}

TEST(ConfigTest, UnknownUpstreamReferenceFails) {
    TempFile file(R"({
        "upstreams": {
            "up1": { "kind": "openai-chat", "base_url": "http://127.0.0.1:10010/v1", "model": "m" }
        },
        "models": { "qwen": { "targets": ["nope"] } }
    })");
    auto cfg = gw::gateway::Config::load(file.path());
    ASSERT_FALSE(cfg.has_value());
    EXPECT_NE(cfg.error().find("nope"), std::string::npos);
}

TEST(ConfigTest, MeteringConstraint) {
    TempFile file(R"({
        "upstreams": {
            "up1": { "kind": "anthropic", "base_url": "http://127.0.0.1:10010/v1", "model": "m",
                     "metering": "llamacpp" }
        },
        "models": { "qwen": { "targets": ["up1"] } }
    })");
    auto cfg = gw::gateway::Config::load(file.path());
    ASSERT_FALSE(cfg.has_value());
}

TEST(ConfigTest, QuotaRequiresRedis) {
    TempFile file(R"({
        "users": { "gk-a": { "name": "a", "quota": { "daily_tokens": 100 } } },
        "upstreams": {
            "up1": { "kind": "openai-chat", "base_url": "http://127.0.0.1:10010/v1", "model": "m" }
        },
        "models": { "qwen": { "targets": ["up1"] } }
    })");
    auto cfg = gw::gateway::Config::load(file.path());
    ASSERT_FALSE(cfg.has_value());
}

TEST(ConfigTest, RedisRequiresPostgres) {
    TempFile file(R"({
        "redis": { "host": "127.0.0.1", "port": 6380 },
        "upstreams": {
            "up1": { "kind": "openai-chat", "base_url": "http://127.0.0.1:10010/v1", "model": "m" }
        },
        "models": { "qwen": { "targets": ["up1"] } }
    })");
    auto cfg = gw::gateway::Config::load(file.path());
    ASSERT_FALSE(cfg.has_value());
}

TEST(ConfigTest, ParseListenAddress) {
    auto a = gw::gateway::parseListenAddress("0.0.0.0:8080");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->first, "0.0.0.0");
    EXPECT_EQ(a->second, 8080);

    auto b = gw::gateway::parseListenAddress("[::1]:9090");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->first, "::1");
    EXPECT_EQ(b->second, 9090);

    EXPECT_FALSE(gw::gateway::parseListenAddress("8080").has_value());
    EXPECT_FALSE(gw::gateway::parseListenAddress("host:0").has_value());
}
