// Defines DAG-specific graph invoke runtime state.
#pragma once

#include "wh/compose/graph/detail/runtime/invoke_session.hpp"

namespace wh::compose::detail::invoke_runtime {

class dag_runtime final {
public:
  explicit dag_runtime(invoke_session session) : session_(std::move(session)) {
    dag_node_phases_.resize(session_.node_count(),
                            input_runtime::dag_node_phase::pending);
    dag_schedule_.reset(session_.node_count());
    frontier_.reset(session_.node_count());
    suspended_.reset(session_.node_count(), false);
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

  auto enqueue_dependents(const std::uint32_t source_node_id) -> void;

  [[nodiscard]] auto promote_next_wave() -> bool;

  [[nodiscard]] auto capture_pending_inputs() -> graph_sender;

  template <typename enqueue_fn_t>
  auto commit_node_output(attempt_id attempt, graph_value node_output,
                          enqueue_fn_t &&enqueue_fn)
      -> wh::core::result<void>;

  [[nodiscard]] auto take_ready_action() -> ready_action;

  auto rebind_moved_runtime_storage() noexcept -> void {
    session_.rebind_moved_runtime_storage();
  }

  [[nodiscard]] auto session() noexcept -> invoke_session & { return session_; }

  [[nodiscard]] auto session() const noexcept -> const invoke_session & {
    return session_;
  }

  auto dag_node_phases() -> std::vector<input_runtime::dag_node_phase> & {
    return dag_node_phases_;
  }

  [[nodiscard]] auto dag_node_phases() const noexcept
      -> const std::vector<input_runtime::dag_node_phase> & {
    return dag_node_phases_;
  }

  auto try_persist_checkpoint() -> void;

  auto make_freeze_sender(graph_sender capture_sender,
                          const bool external_interrupt) -> graph_sender {
    return session_.make_freeze_sender(
        std::move(capture_sender), external_interrupt,
        [this]() { try_persist_checkpoint(); });
  }

  auto mark_suspended(const std::uint32_t node_id) -> void {
    if (suspended_.set_if_unset(node_id)) {
      suspended_nodes_.push_back(node_id);
    }
  }

  [[nodiscard]] auto capture_suspended_nodes() const
      -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> captured{};
    captured.reserve(suspended_nodes_.size());
    for (const auto node_id : suspended_nodes_) {
      if (node_id < suspended_.size() && suspended_.test(node_id) &&
          node_id < dag_node_phases_.size() &&
          dag_node_phases_[node_id] == input_runtime::dag_node_phase::pending) {
        captured.push_back(node_id);
      }
    }
    return captured;
  }

  auto restore_suspended_nodes(std::vector<std::uint32_t> suspended_nodes)
      -> void {
    suspended_nodes_ = std::move(suspended_nodes);
    suspended_.reset(session_.node_count(), false);
    for (const auto node_id : suspended_nodes_) {
      suspended_.set(node_id);
    }
  }

  auto branch_states() -> std::vector<input_runtime::branch_state> & {
    return dag_schedule_.branch_states;
  }

  auto frontier() -> detail::dag_frontier & { return frontier_; }

private:
  invoke_session session_;
  std::vector<input_runtime::dag_node_phase> dag_node_phases_{};
  input_runtime::dag_schedule dag_schedule_{};
  detail::dag_frontier frontier_{};
  detail::dynamic_bitset suspended_{};
  std::vector<std::uint32_t> suspended_nodes_{};
};

} // namespace wh::compose::detail::invoke_runtime
