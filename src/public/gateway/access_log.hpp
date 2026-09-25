#pragma once

#include "base/noncopyable.hpp"
#include "gateway/request_record.hpp"

namespace gw::gateway {

// 访问日志：每请求一行 JSON（spdlog 异步 logger + rotating_file_sink，policies.md §3.4）。
// spdlog 的异步线程池与文件 sink 在 gw::log::init 里已经建好，这里只是序列化与投递。
class AccessLog : noncopyable {
public:
    void write(const RequestRecord& record) const;
};

}  // namespace gw::gateway
