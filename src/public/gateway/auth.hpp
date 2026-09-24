#pragma once

#include <optional>
#include <string>

#include "gateway/config.hpp"
#include "http/http_request.hpp"

namespace gw::gateway {

// Authorization: Bearer <key> 或 x-api-key: <key>
std::optional<std::string> extractApiKey(const http::HttpRequest& request);

struct AuthResult {
    const UserConfig* user = nullptr;
    std::string message;
};

AuthResult authenticate(const Config& config, const http::HttpRequest& request);

}  // namespace gw::gateway
