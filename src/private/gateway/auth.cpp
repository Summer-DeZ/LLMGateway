#include "gateway/auth.hpp"

#include "base/strings.hpp"
#include "gateway/key_store.hpp"

namespace gw::gateway {

std::optional<std::string> extractApiKey(const http::HttpRequest& request) {
    if (auto auth = request.header("authorization")) {
        auto sp = auth->find(' ');
        if (sp != std::string_view::npos) {
            std::string_view scheme = auth->substr(0, sp);
            std::string_view token = auth->substr(sp + 1);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
            if (iequals(scheme, "bearer") && !token.empty()) return std::string(token);
        }
    }
    if (auto key = request.header("x-api-key")) {
        if (!key->empty()) return std::string(*key);
    }
    return std::nullopt;
}

AuthResult authenticate(const Config& config, const http::HttpRequest& request, const KeyRegistry* keys) {
    auto key = extractApiKey(request);
    if (!key) return {nullptr, "missing api key"};
    if (keys) {
        auto identity = keys->find(*key);
        if (identity.user) {
            if (!keys->ready) return {nullptr, "key service unavailable", 503};
            if (!identity.enabled) return {nullptr, "invalid api key"};
            return {std::move(identity.user), {}};
        }
    }
    const UserConfig* user = config.findUser(*key);
    if (!user && keys && !keys->ready) return {nullptr, "key service unavailable", 503};
    if (user == nullptr) return {nullptr, "invalid api key"};
    return {std::make_shared<const UserConfig>(*user), {}};
}

}  // namespace gw::gateway
