// Defines the public Pregel compose entrypoint.
#pragma once

#include "wh/compose/graph/mode.hpp"

namespace wh::compose {

/// Pregel-specialized compose entrypoint.
using pregel = mode_graph<graph_runtime_mode::pregel>;

} // namespace wh::compose
