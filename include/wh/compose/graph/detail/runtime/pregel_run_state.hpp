// Defines Pregel-specific graph invoke runtime state.
#pragma once

#include "wh/compose/graph/detail/runtime/run_state.hpp"

namespace wh::compose::detail::invoke_runtime {

class pregel_run_state final : public run_state {
public:
  explicit pregel_run_state(run_state &&state) : run_state(std::move(state)) {
    pregel_delivery_.reset(node_count());
  }

  auto initialize_pregel_entry() -> void;

  [[nodiscard]] auto make_input_sender(node_frame *frame) -> graph_sender;

  [[nodiscard]] auto capture_pregel_pending_inputs() -> graph_sender;

  template <typename enqueue_fn_t>
  auto commit_pregel_node_output(node_frame &&frame, graph_value node_output,
                                 enqueue_fn_t &&enqueue_fn)
      -> wh::core::result<void>;

  auto commit_pregel_skip_action(const pregel_action &action)
      -> wh::core::result<void>;

  [[nodiscard]] auto take_next_pregel_action(const std::uint32_t node_id,
                                             const std::size_t step)
      -> pregel_action;

  auto pregel_delivery() -> input_runtime::pregel_delivery_store & {
    return pregel_delivery_;
  }

private:
  input_runtime::pregel_delivery_store pregel_delivery_{};
};

} // namespace wh::compose::detail::invoke_runtime
