#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "base/timestamp.hpp"

namespace gw::net {

class Buffer;
class TcpConnection;

using TimerCallback = std::function<void()>;
using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*, Timestamp)>;
using WriteCompleteCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
using HighWaterMarkCallback = std::function<void(const std::shared_ptr<TcpConnection>&, std::size_t)>;
using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

}  // namespace gw::net
