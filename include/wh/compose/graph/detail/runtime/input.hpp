// Defines shared runtime input aliases over the split common/DAG runtime types.
#pragma once

#include "wh/compose/graph/detail/runtime/common_input_types.hpp"
#include "wh/compose/graph/detail/runtime/dag_types.hpp"
#include "wh/compose/graph/detail/runtime/pregel_types.hpp"
#include "wh/compose/graph/detail/runtime/value_input_ops.hpp"

namespace wh::compose::detail::input_runtime {

using edge_status = dag_edge_status;
using ready_state = dag_ready_state;
using branch_state = dag_branch_state;
using io_storage = runtime_io_storage;
using dag_schedule = dag_schedule_state;

} // namespace wh::compose::detail::input_runtime
