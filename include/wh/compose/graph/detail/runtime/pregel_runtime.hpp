// Defines Pregel-specific graph invoke runtime state.
#pragma once

#include "wh/compose/graph/detail/runtime/invoke_session.hpp"

namespace wh::compose::detail::invoke_runtime {

class pregel_runtime final {
public:
  explicit pregel_runtime(invoke_session session) : session_(std::move(session)) {
    pregel_delivery_.reset(session_.node_count());
    superstep_active_ = false;
  }

  [[nodiscard]] auto capture_checkpoint_state()
      -> wh::core::result<checkpoint_state>;
  [[nodiscard]] auto capture_checkpoint_runtime(checkpoint_runtime_state &runtime)
      -> wh::core::result<void>;

  auto initialize_entry() -> void;
  [[nodiscard]] auto restore_entry(
      detail::checkpoint_runtime::prepared_restore &prepared)
      -> wh::core::result<void>;
  [[nodiscard]] auto start_entry(graph_value input) -> wh::core::result<void>;

  [[nodiscard]] auto make_input_sender(attempt_id attempt) -> graph_sender;
  [[nodiscard]] auto make_input_attempt(const std::uint32_t node_id,
                                        std::size_t step)
      -> wh::core::result<attempt_id>;
  [[nodiscard]] auto begin_state_pre(attempt_id attempt)
      -> wh::core::result<state_step>;
  auto commit_terminal_input(attempt_id attempt, graph_value input)
      -> wh::core::result<void>;

  [[nodiscard]] auto finish() -> wh::core::result<graph_value>;

  [[nodiscard]] auto capture_pending_inputs() -> graph_sender;

  template <typename enqueue_fn_t>
  auto commit_node_output(attempt_id attempt, graph_value node_output,
                          enqueue_fn_t &&enqueue_fn)
      -> wh::core::result<void>;

  auto commit_skip_action(const pregel_action &action)
      -> wh::core::result<void>;

  [[nodiscard]] auto take_ready_action(const std::uint32_t node_id,
                                       const std::size_t step)
      -> pregel_action;

  auto rebind_moved_runtime_storage() noexcept -> void {
    session_.rebind_moved_runtime_storage();
  }

  [[nodiscard]] auto session() noexcept -> invoke_session & { return session_; }

  [[nodiscard]] auto session() const noexcept -> const invoke_session & {
    return session_;
  }

  auto try_persist_checkpoint() -> void;

  auto make_freeze_sender(graph_sender capture_sender,
                          const bool external_interrupt) -> graph_sender {
    return session_.make_freeze_sender(
        std::move(capture_sender), external_interrupt,
        [this]() { try_persist_checkpoint(); });
  }

  auto pregel_delivery() -> input_runtime::pregel_delivery_store & {
    return pregel_delivery_;
  }

  [[nodiscard]] auto superstep_active() const noexcept -> bool {
    return superstep_active_;
  }

  auto set_superstep_active(const bool active) noexcept -> void {
    superstep_active_ = active;
  }

private:
  invoke_session session_;
  input_runtime::pregel_delivery_store pregel_delivery_{};
  bool superstep_active_{false};
};

} // namespace wh::compose::detail::invoke_runtime
