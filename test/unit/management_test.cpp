#include "gateway/management.hpp"

#include <gtest/gtest.h>

#include "gateway/auth.hpp"

using namespace gw::gateway;

TEST(ManagementTest, CredentialsUseDigestAndStableIdentity) {
    EXPECT_EQ(keyDigest(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    auto secret = generateApiKey();
    EXPECT_EQ(secret.size(), 68u);
    EXPECT_TRUE(secret.starts_with("gwk_"));
    EXPECT_TRUE(managementId(generateKeyId()));
    Config config;
    KeyRegistry registry;
    auto user = std::make_shared<UserConfig>();
    user->name = "managed:stable";
    auto snapshot = std::make_shared<KeyRegistry::Snapshot>();
    (*snapshot)[keyDigest(secret)] = {user, true};
    registry.publish(snapshot);
    registry.ready = true;
    gw::http::HttpRequest request;
    request.headers = {{"authorization", "Bearer " + secret}};
    auto admitted = authenticate(config, request, &registry);
    ASSERT_NE(admitted.user, nullptr);
    EXPECT_EQ(admitted.user->name, "managed:stable");
    auto disabled = std::make_shared<KeyRegistry::Snapshot>(*snapshot);
    (*disabled)[keyDigest(secret)].enabled = false;
    registry.publish(disabled);
    EXPECT_EQ(authenticate(config, request, &registry).user, nullptr);
    EXPECT_EQ(admitted.user->name, "managed:stable");  // Already-admitted request owns its principal.
    registry.ready = false;
    EXPECT_EQ(authenticate(config, request, &registry).status, 503);
}

TEST(ManagementTest, LocalSourceAndSameOriginRequired) {
    gw::http::HttpRequest request;
    request.method = "GET";
    request.headers = {{"host", "127.0.0.1:8080"}};
    EXPECT_TRUE(managementSourceAllowed(request, "127.0.0.1", 8080));
    EXPECT_TRUE(managementSourceAllowed(request, "::ffff:127.0.0.1", 8080));
    EXPECT_FALSE(managementSourceAllowed(request, "10.0.0.1", 8080));
    EXPECT_FALSE(managementSourceAllowed(request, "127.0.0.1", 8081));
    request.method = "POST";
    EXPECT_FALSE(managementSourceAllowed(request, "127.0.0.1", 8080));
    request.headers.emplace_back("origin", "http://127.0.0.1:8080");
    request.headers.emplace_back("content-type", "application/json");
    request.headers.emplace_back("x-gateway-management", "1");
    EXPECT_TRUE(managementSourceAllowed(request, "127.0.0.1", 8080));
    request.headers.emplace_back("origin", "http://elsewhere.invalid");
    EXPECT_FALSE(managementSourceAllowed(request, "127.0.0.1", 8080));
}

TEST(ManagementTest, StrictQueryAndCalendarValidation) {
    EXPECT_EQ(managementQuery("start=2026-09-25&name=%E4%B8%AD+abc").at("name"), "中 abc");
    EXPECT_THROW(managementQuery("start=a&start=b"), std::invalid_argument);
    EXPECT_THROW(managementQuery("cursor=%xx"), std::invalid_argument);
    EXPECT_THROW(managementDates("2026-02-29", "2026-03-01"), std::invalid_argument);
    EXPECT_THROW(managementDates("2026-09-26", "2026-09-25"), std::invalid_argument);
    EXPECT_THROW(managementDates("2024-01-01", "2025-01-01"), std::invalid_argument);
    EXPECT_NO_THROW(managementDates("2024-02-29", "2024-03-01"));
}

TEST(ManagementTest, NamesHaveUnicodeLengthAndRejectControls) {
    EXPECT_EQ(managementName({{"name", "  开发客户端  "}}), "开发客户端");
    EXPECT_THROW(managementName({{"name", "   "}}), std::invalid_argument);
    EXPECT_THROW(managementName({{"name", "x\ny"}}), std::invalid_argument);
    EXPECT_THROW(managementName({{"name", std::string(65, 'a')}}), std::invalid_argument);
    EXPECT_THROW(managementName({{"name", "x"}, {"secret", "y"}}), std::invalid_argument);
}
