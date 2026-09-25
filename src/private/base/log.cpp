#include "base/log.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <vector>

namespace gw::log {

namespace {

std::shared_ptr<spdlog::logger> g_access;

spdlog::level::level_enum parseLevel(const std::string& level) {
    std::string l = level;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (l == "trace") return spdlog::level::trace;
    if (l == "debug") return spdlog::level::debug;
    if (l == "warn" || l == "warning") return spdlog::level::warn;
    if (l == "error" || l == "err") return spdlog::level::err;
    if (l == "critical") return spdlog::level::critical;
    if (l == "off") return spdlog::level::off;
    return spdlog::level::info;
}

}  // namespace

void init(const std::string& level, const std::string& file, const std::string& access_file) {
    spdlog::init_thread_pool(8192, 1);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    if (!file.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file, 50 * 1024 * 1024, 5));
    }
    auto logger = std::make_shared<spdlog::async_logger>(
        "gw", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    logger->set_level(parseLevel(level));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e [%t] %^%l%$ %v");
    spdlog::flush_every(std::chrono::seconds(1));

    if (!access_file.empty()) {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(access_file, 50 * 1024 * 1024, 5);
        g_access = std::make_shared<spdlog::async_logger>(
            "gw.access", sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
        g_access->set_level(spdlog::level::info);
        g_access->set_pattern("%v");
        // 访问日志一行一请求，逐条刷盘：审计与验收要求请求结束后立刻可见
        g_access->flush_on(spdlog::level::info);
    }
}

std::shared_ptr<spdlog::logger> accessLogger() {
    return g_access;
}

void shutdown() {
    if (g_access) {
        g_access->flush();
        g_access.reset();
    }
    spdlog::shutdown();
}

}  // namespace gw::log
