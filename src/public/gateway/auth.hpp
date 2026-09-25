#pragma once

#include <optional>
#include <string>

#include "gateway/config.hpp"
#include "http/http_request.hpp"

namespace gw::gateway {

// Authorization: Bearer <key> 或 x-api-key: <key>
std::optional<std::string> extractApiKey(const http::HttpRequest& request);

class KeyRegistry;

struct AuthResult {
    std::shared_ptr<const UserConfig> user;
    std::string message;
    int status = 401;
};

AuthResult authenticate(const Config& config, const http::HttpRequest& request, const KeyRegistry* keys = nullptr);

}  // namespace gw::gateway
