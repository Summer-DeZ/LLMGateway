#include "proto/errors.hpp"

namespace gw::proto {

std::string openAiErrorBody(std::string_view message, std::string_view type, std::string_view code) {
    nlohmann::json j;
    j["error"]["message"] = message;
    j["error"]["type"] = type;
    j["error"]["code"] = code.empty() ? nlohmann::json(nullptr) : nlohmann::json(std::string(code));
    j["error"]["param"] = nullptr;
    return j.dump();
}

std::string anthropicErrorBody(std::string_view message, std::string_view type) {
    nlohmann::json j;
    j["type"] = "error";
    j["error"]["type"] = type;
    j["error"]["message"] = message;
    return j.dump();
}

std::string errorBody(Protocol protocol, std::string_view message, std::string_view type, std::string_view code) {
    if (protocol == Protocol::anthropic) {
        return anthropicErrorBody(message, type);
    }
    return openAiErrorBody(message, type, code);
}

std::string extractErrorMessage(std::string_view body) {
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("error") && j.at("error").is_object()) {
            const auto& error = j.at("error");
            if (error.contains("message") && error.at("message").is_string()) {
                return error.at("message").get<std::string>();
            }
        }
        if (j.contains("message") && j.at("message").is_string()) {
            return j.at("message").get<std::string>();
        }
    } catch (...) {
    }
    if (body.size() > 1024) return std::string(body.substr(0, 1024));
    return std::string(body);
}

}  // namespace gw::proto
