#pragma once

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

namespace gw::log {

// 初始化运行日志（控制台 + 滚动文件）与访问日志（异步滚动文件）。
// level: trace/debug/info/warn/error/critical；file 为空则只输出控制台。
void init(const std::string& level, const std::string& file, const std::string& access_file);

// 访问日志 logger（每请求一行 JSON），init 之前返回 nullptr。
std::shared_ptr<spdlog::logger> accessLogger();

void shutdown();

}  // namespace gw::log
