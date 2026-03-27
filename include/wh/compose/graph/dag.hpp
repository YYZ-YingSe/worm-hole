// Defines the public DAG compose entrypoint.
#pragma once

#include "wh/compose/graph/mode.hpp"

namespace wh::compose {

/// DAG-specialized compose entrypoint.
using dag = mode_graph<graph_runtime_mode::dag>;

} // namespace wh::compose
