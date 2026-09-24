#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "proto/neutral.hpp"

namespace gw::proto {

std::string openAiErrorBody(std::string_view message, std::string_view type, std::string_view code = "");
std::string anthropicErrorBody(std::string_view message, std::string_view type);
std::string errorBody(Protocol protocol, std::string_view message, std::string_view type, std::string_view code = "");
// 从上游错误体提取 message（OpenAI error.message / Anthropic error.message / 原始文本前 1KB）
std::string extractErrorMessage(std::string_view body);

}  // namespace gw::proto
