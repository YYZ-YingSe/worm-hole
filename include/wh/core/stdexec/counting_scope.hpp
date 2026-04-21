// Re-exports stdexec counting scopes for internal operation-state barriers.
#pragma once

#include <stdexec/__detail/__counting_scopes.hpp>

namespace wh::core::detail {

using simple_counting_scope = stdexec::simple_counting_scope;
using counting_scope = stdexec::counting_scope;

} // namespace wh::core::detail
