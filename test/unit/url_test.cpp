#include "http/url.hpp"

#include <gtest/gtest.h>

using gw::http::parseBaseUrl;

TEST(UrlTest, HttpDefaultPort) {
    auto url = parseBaseUrl("http://localhost");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->host, "localhost");
    EXPECT_EQ(url->port, 80);
    EXPECT_EQ(url->path_prefix, "");
}

TEST(UrlTest, HttpWithPortAndPrefix) {
    auto url = parseBaseUrl("http://127.0.0.1:10010/v1");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->host, "127.0.0.1");
    EXPECT_EQ(url->port, 10010);
    EXPECT_EQ(url->path_prefix, "/v1");
}

TEST(UrlTest, TrailingSlashRemoved) {
    auto url = parseBaseUrl("http://example.com/v1/");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->path_prefix, "/v1");

    auto root = parseBaseUrl("http://example.com/");
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(root->path_prefix, "");
}

TEST(UrlTest, Ipv6Literal) {
    auto url = parseBaseUrl("http://[::1]:8080/v1");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->host, "::1");
    EXPECT_EQ(url->port, 8080);
    EXPECT_EQ(url->path_prefix, "/v1");

    auto no_port = parseBaseUrl("http://[2001:db8::1]/api");
    ASSERT_TRUE(no_port.has_value());
    EXPECT_EQ(no_port->host, "2001:db8::1");
    EXPECT_EQ(no_port->port, 80);
    EXPECT_EQ(no_port->path_prefix, "/api");
}

TEST(UrlTest, Invalid) {
    EXPECT_FALSE(parseBaseUrl("ftp://example.com").has_value());
    EXPECT_FALSE(parseBaseUrl("https://api.example.com").has_value());
    EXPECT_FALSE(parseBaseUrl("example.com").has_value());
    EXPECT_FALSE(parseBaseUrl("http://").has_value());
    EXPECT_FALSE(parseBaseUrl("http://host:99999").has_value());
    EXPECT_FALSE(parseBaseUrl("http://host:abc").has_value());
    EXPECT_FALSE(parseBaseUrl("http://user:pass@host/").has_value());
}
