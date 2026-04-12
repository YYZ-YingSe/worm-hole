#pragma once

#include <mutex>

namespace wh::core::detail {

using bounded_queue_critical_section = std::mutex;

} // namespace wh::core::detail
