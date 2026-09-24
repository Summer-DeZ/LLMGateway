#pragma once

#include <atomic>
#include <memory>

#include "gateway/config.hpp"

namespace gw::net {
class Resolver;
}  // namespace gw::net

namespace gw::gateway {

// main 栈上，生命周期覆盖所有 loop。后续任务在这里挂共享服务。
struct GatewayContext {
    std::shared_ptr<const Config> config;
    std::atomic<int> active_sessions{0};
    std::atomic<bool> draining{false};
    net::Resolver* resolver = nullptr;
};

}  // namespace gw::gateway
