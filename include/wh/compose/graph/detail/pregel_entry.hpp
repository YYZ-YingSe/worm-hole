// Defines Pregel-specific graph-run entry initialization.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/pregel_run_state.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::pregel_run_state::initialize_pregel_entry()
    -> void {
  const auto &index = compiled_graph_index();
  owner_->reset_pregel_source_caches(index.start_id, io_storage_);
  owner_->seed_pregel_successors(index.start_id,
                                 invoke_state().start_entry_selection,
                                 pregel_delivery_);
  for (const auto node_id : index.allow_no_control_ids) {
    pregel_delivery_.stage_current_node(node_id);
  }
  invoke_state().start_entry_selection.reset();
}

} // namespace wh::compose
