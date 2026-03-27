// Defines the public compose graph facade and inline implementation includes.
#pragma once

#include "wh/compose/graph/class.hpp"
#include "wh/compose/graph/detail/runtime/run_state.hpp"

#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/control.hpp"
#include "wh/compose/graph/detail/input_collect.hpp"
#include "wh/compose/graph/detail/input.hpp"
#include "wh/compose/graph/detail/pending_input.hpp"
#include "wh/compose/graph/detail/policy.hpp"
#include "wh/compose/graph/detail/stream_rewrite.hpp"
#include "wh/compose/graph/detail/state_phase.hpp"
#include "wh/compose/graph/detail/invoke.hpp"
#include "wh/compose/graph/detail/invoke_common.hpp"
#include "wh/compose/graph/detail/dag.hpp"
#include "wh/compose/graph/detail/dag_loop.hpp"
#include "wh/compose/graph/detail/invoke_join.hpp"
#include "wh/compose/graph/detail/pregel_loop.hpp"
#include "wh/compose/graph/detail/pregel.hpp"
#include "wh/compose/graph/detail/start.hpp"
