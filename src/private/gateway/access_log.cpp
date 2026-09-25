#include "gateway/access_log.hpp"

#include "base/log.hpp"

namespace gw::gateway {

void AccessLog::write(const RequestRecord& record) const {
    if (auto logger = gw::log::accessLogger()) {
        logger->info(record.toJson().dump());
    }
}

}  // namespace gw::gateway
