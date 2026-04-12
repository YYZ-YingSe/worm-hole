// Defines DAG-specific graph invoke runtime state.
#pragma once

#include "wh/compose/graph/detail/runtime/run_state.hpp"

namespace wh::compose::detail::invoke_runtime {

class dag_run_state final : public run_state {
public:
  explicit dag_run_state(run_state &&state) : run_state(std::move(state)) {
    dag_schedule_.reset(node_count());
    frontier_.reset(node_count());
  }

  auto initialize_dag_entry() -> void;

  [[nodiscard]] auto make_input_sender(node_frame *frame) -> graph_sender;

  auto enqueue_dependents(const std::uint32_t source_node_id) -> void;

  [[nodiscard]] auto promote_next_wave() -> bool;

  [[nodiscard]] auto capture_dag_pending_inputs() -> graph_sender;

  template <typename enqueue_fn_t>
  auto commit_dag_node_output(node_frame &&frame, graph_value node_output,
                              enqueue_fn_t &&enqueue_fn)
      -> wh::core::result<void>;

  [[nodiscard]] auto take_next_ready_action() -> ready_action;

  auto branch_states() -> std::vector<input_runtime::branch_state> & {
    return dag_schedule_.branch_states;
  }

  auto frontier() -> detail::dag_frontier & { return frontier_; }

private:
  input_runtime::dag_schedule dag_schedule_{};
  detail::dag_frontier frontier_{};
};

} // namespace wh::compose::detail::invoke_runtime
