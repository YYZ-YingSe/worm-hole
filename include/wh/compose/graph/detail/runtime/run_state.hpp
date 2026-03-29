// Defines the compose graph invoke runtime state machine declaration.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <exec/timed_thread_scheduler.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/graph/class.hpp"
#include "wh/compose/graph/detail/bitset.hpp"
#include "wh/compose/graph/detail/runtime/input.hpp"
#include "wh/compose/graph/detail/runtime/invoke.hpp"
#include "wh/compose/graph/detail/runtime/process.hpp"
#include "wh/compose/graph/detail/runtime/rerun.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/address.hpp"
#include "wh/core/error.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"

namespace wh::compose::detail::invoke_runtime {

template <typename receiver_t> class runtime_operation;
class runtime_sender;

class run_state {
public:
  using dynamic_bitset = detail::dynamic_bitset;
  using edge_status = input_runtime::edge_status;
  using ready_state = input_runtime::ready_state;
  using input_lane = input_runtime::input_lane;
  using branch_state = input_runtime::branch_state;
  using reader_lane_state = input_runtime::reader_lane_state;
  using reader_lowering = input_runtime::reader_lowering;
  using scratch_buffer = input_runtime::scratch_buffer;
  using resolved_input = input_runtime::resolved_input;
  using value_input = input_runtime::value_input;
  using value_batch = input_runtime::value_batch;
  using node_state = input_runtime::node_state;
  using node_frame = detail::invoke_runtime::node_frame;
  using state_step = detail::invoke_runtime::state_step;
  using invoke_stage = detail::invoke_runtime::stage;
  using ready_action_kind = detail::invoke_runtime::ready_action_kind;
  using ready_action = detail::invoke_runtime::ready_action;
  using pregel_action = detail::invoke_runtime::pregel_action;

  run_state(const graph *owner, graph_value &&input, wh::core::run_context &context,
            graph_call_options &&call_options,
            wh::core::detail::any_resume_scheduler_t graph_scheduler,
            node_path path_prefix = {},
            graph_process_state *parent_process_state = nullptr,
            detail::runtime_state::invoke_outputs *nested_outputs = nullptr,
            const graph_runtime_services *services = nullptr,
            graph_invoke_controls controls = {},
            detail::runtime_state::invoke_outputs *published_outputs = nullptr,
            const run_state *parent_state = nullptr);

  run_state(const graph *owner, graph_value &&input, wh::core::run_context &context,
            graph_call_scope call_scope,
            wh::core::detail::any_resume_scheduler_t graph_scheduler,
            node_path path_prefix = {},
            graph_process_state *parent_process_state = nullptr,
            detail::runtime_state::invoke_outputs *nested_outputs = nullptr,
            const graph_runtime_services *services = nullptr,
            graph_invoke_controls controls = {},
            detail::runtime_state::invoke_outputs *published_outputs = nullptr,
            const run_state *parent_state = nullptr);

  [[nodiscard]] static auto start(run_state &&state) -> graph_sender;

private:
  auto rebind_moved_runtime_storage() noexcept -> void;

  auto initialize_runtime_node_caches() -> void;

  [[nodiscard]] auto runtime_node_path(const std::uint32_t node_id)
      -> const node_path &;

  [[nodiscard]] auto runtime_stream_scope(const std::uint32_t node_id)
      -> const graph_event_scope &;

  [[nodiscard]] auto runtime_node_execution_address(const std::uint32_t node_id)
      -> const wh::core::address &;

  auto initialize_resolved_component_options() -> void;

  auto initialize_resolved_node_observations() -> void;

  [[nodiscard]] auto initialize_resolved_state_handlers() -> wh::core::result<void>;

  auto initialize_trace_state() -> void;

  [[nodiscard]] auto next_node_trace(const std::uint32_t node_id)
      -> graph_node_trace;

  [[nodiscard]] auto transition_log() noexcept -> graph_transition_log &;

  auto initialize(graph_value &&input, graph_call_scope call_scope) -> void;

  [[nodiscard]] auto immediate_success(graph_value value) -> graph_sender;

  [[nodiscard]] auto immediate_failure(const wh::core::error_code code)
      -> graph_sender;

  auto persist_checkpoint_best_effort() -> void;

  auto publish_runtime_outputs() -> void;

  auto emit_debug(const graph_debug_stream_event::decision_kind decision,
                  const std::uint32_t node_id, const std::size_t step) -> void;

  auto append_transition(const graph_state_transition_event &event) -> void;

  auto append_transition(graph_state_transition_event &&event) -> void;

  auto append_transition(const std::uint32_t node_id,
                         const graph_state_transition_event &event) -> void;

  auto append_transition(const std::uint32_t node_id,
                         graph_state_transition_event &&event) -> void;

  [[nodiscard]] auto evaluate_resume_match(const std::uint32_t node_id)
      -> wh::core::result<std::optional<wh::core::interrupt_signal>>;

  [[nodiscard]] auto control_slot_id() const noexcept -> std::uint32_t;

  auto request_freeze(const bool external_interrupt) noexcept -> void;

  [[nodiscard]] auto freeze_requested() const noexcept -> bool;

  [[nodiscard]] auto freeze_external() const noexcept -> bool;

  [[nodiscard]] auto freeze_sender(const bool external_interrupt) -> graph_sender;

  auto flush_deferred_enqueue() -> void;

  auto enqueue_dependents(const std::uint32_t source_node_id) -> void;

  auto enqueue_pregel_dependents(const std::uint32_t source_node_id,
                                 std::vector<std::uint32_t> &frontier,
                                 detail::dynamic_bitset &frontier_queued) -> void;

  [[nodiscard]] auto capture_pending_inputs() -> graph_sender;

  [[nodiscard]] auto check_external_interrupt_boundary()
      -> wh::core::result<bool>;

  static auto bind_node_runtime_call_options(
      node_frame &frame, const graph_call_scope &bound_call_options,
      run_state *state) noexcept -> void;

  [[nodiscard]] static auto start_nested_from_runtime(
      const void *state_ptr, const graph &nested_graph,
      wh::core::run_context &context, graph_value &input,
      const graph_call_scope *call_options, const node_path *path_prefix,
      graph_process_state *parent_process_state,
      detail::runtime_state::invoke_outputs *nested_outputs,
      const graph_node_trace *parent_trace) -> graph_sender;

  [[nodiscard]] auto nested_graph_entry() const noexcept
      -> wh::compose::nested_graph_entry;

  [[nodiscard]] static auto timeout_scheduler() noexcept
      -> exec::timed_thread_scheduler;

  [[nodiscard]] auto make_input_frame(const std::uint32_t node_id, std::size_t step)
      -> wh::core::result<node_frame>;

  [[nodiscard]] auto begin_state_pre(node_frame &&frame, graph_value input)
      -> wh::core::result<state_step>;

  [[nodiscard]] auto prepare_execution_input(node_frame &&frame, graph_value input)
      -> wh::core::result<state_step>;

  [[nodiscard]] auto should_retain_input(const node_frame &frame) const noexcept
      -> bool;

  [[nodiscard]] auto finalize_node_frame(node_frame &&frame, graph_value input)
      -> wh::core::result<node_frame>;

  [[nodiscard]] auto begin_state_post(node_frame &&frame, graph_value output)
      -> wh::core::result<state_step>;

  auto fail_node_stage(node_frame &&frame, wh::core::error_code code,
                       std::string_view message) -> wh::core::result<void>;

  [[nodiscard]] static auto make_node_timeout_failure(
      detail::runtime_state::invoke_outputs &outputs,
      const std::string_view node_key, const std::size_t attempt,
      const std::chrono::milliseconds timeout_budget,
      const std::chrono::steady_clock::time_point attempt_start)
      -> wh::core::result<graph_value>;

  [[nodiscard]] static auto apply_node_timeout(
      detail::runtime_state::invoke_outputs &outputs, const node_frame &frame,
      const std::chrono::steady_clock::time_point attempt_start,
      wh::core::result<graph_value> executed) -> wh::core::result<graph_value>;

  template <stdexec::sender sender_t>
  [[nodiscard]] static auto make_async_timed_node_sender(
      sender_t &&sender, detail::runtime_state::invoke_outputs &outputs,
      const node_frame &frame,
      const std::chrono::steady_clock::time_point attempt_start) -> graph_sender;

  template <typename retry_fn_t>
  [[nodiscard]] static auto run_sync_node_execution(
      const compiled_node &node, graph_value &input_value,
      wh::core::run_context &context,
      const graph_call_scope &bound_call_options, run_state *state,
      node_frame &frame, retry_fn_t retry_fn) -> wh::core::result<graph_value>;

  [[nodiscard]] static auto make_async_node_attempt_sender(
      const compiled_node &node, graph_value &input_value,
      wh::core::run_context &context,
      const graph_call_scope &bound_call_options, run_state *state,
      node_frame &frame) -> graph_sender;

  template <typename enqueue_fn_t>
  auto commit_node_output(node_frame &&frame, graph_value node_output,
                          enqueue_fn_t &&enqueue_fn) -> wh::core::result<void>;

  auto commit_pregel_skip_action(const pregel_action &action,
                                 std::vector<std::uint32_t> &next_frontier,
                                 detail::dynamic_bitset &next_frontier_queued)
      -> wh::core::result<void>;

  [[nodiscard]] auto take_next_pregel_action(const std::uint32_t node_id,
                                             const std::size_t step)
      -> pregel_action;

  [[nodiscard]] auto take_next_ready_action() -> ready_action;

  [[nodiscard]] auto finish_graph_status() -> wh::core::result<graph_value>;

  [[nodiscard]] auto finish_graph() -> graph_sender;

  [[nodiscard]] auto is_dag() const noexcept -> bool {
    return owner_->options_.mode == graph_runtime_mode::dag;
  }

  [[nodiscard]] auto node_count() const noexcept -> std::size_t {
    return owner_->compiled_execution_index_.index.nodes_by_id.size();
  }

  [[nodiscard]] auto end_id() const noexcept -> std::uint32_t {
    return owner_->compiled_execution_index_.index.end_id;
  }

  [[nodiscard]] auto max_parallel_nodes() const noexcept -> std::size_t {
    return owner_->options_.max_parallel_nodes;
  }

  [[nodiscard]] auto node_key(const std::uint32_t node_id) const
      -> const std::string & {
    return owner_->compiled_execution_index_.index.id_to_key[node_id];
  }

  [[nodiscard]] auto build_input_sender(node_frame *frame) -> graph_sender {
    if (!graph_scheduler_.has_value()) {
      return detail::failure_graph_sender(wh::core::errc::contract_violation);
    }
    return owner_->build_node_input_sender(frame->node_id, scratch_, node_states(),
                                           branch_states(), context_, frame,
                                           invoke_config_, *graph_scheduler_);
  }

  [[nodiscard]] auto store_output(const std::uint32_t node_id, graph_value value)
      -> wh::core::result<void> {
    return owner_->store_node_output(node_id, scratch_, std::move(value));
  }

  [[nodiscard]] auto should_wrap_node_error(
      const wh::core::error_code code) const noexcept -> bool {
    return owner_->should_wrap_as_node_run_error(code);
  }

  auto publish_node_error(const node_path &runtime_path, const std::uint32_t node_id,
                          const wh::core::error_code code,
                          const std::string_view message) -> void {
    owner_->publish_node_run_error(invoke_outputs_, runtime_path, node_id, code,
                                   message);
  }

  auto publish_graph_error(const node_path &runtime_path,
                           const std::string_view node_key_value,
                           const compose_error_phase phase,
                           const wh::core::error_code code,
                           const std::string_view message) -> void {
    owner_->publish_graph_run_error(invoke_outputs_, runtime_path, node_key_value,
                                    phase, code, message);
  }

  [[nodiscard]] auto completed_nodes() const -> std::vector<std::string> {
    return owner_->collect_completed_nodes(scratch_.node_states);
  }

  template <typename receiver_t, typename derived_t, typename graph_scheduler_t>
  class join_base;

  template <typename receiver_t, typename derived_t, typename graph_scheduler_t>
  class stage_run;

  template <typename receiver_t,
            typename graph_scheduler_t =
                wh::core::detail::any_resume_scheduler_t>
  class dag_run;

  template <typename receiver_t,
            typename graph_scheduler_t =
                wh::core::detail::any_resume_scheduler_t>
  class pregel_run;

  template <typename> friend class runtime_operation;
  friend class runtime_sender;

  auto node_values() -> std::vector<graph_value> & { return scratch_.node_values; }

  auto node_readers() -> std::vector<graph_stream_reader> & {
    return scratch_.node_readers;
  }

  auto output_valid() -> detail::dynamic_bitset & { return scratch_.output_valid; }

  auto node_states() -> std::vector<input_runtime::node_state> & {
    return scratch_.node_states;
  }

  auto branch_states() -> std::vector<input_runtime::branch_state> & {
    return scratch_.branch_states;
  }

  auto ready_queue() -> std::vector<std::uint32_t> & {
    return scratch_.ready_queue;
  }

  auto queued() -> detail::dynamic_bitset & { return scratch_.queued; }

  const graph *owner_{nullptr};
  wh::core::run_context &context_;
  std::optional<wh::core::error_code> init_error_{};

  graph_state_table state_table_{};
  graph_process_state process_state_{};
  graph_invoke_controls invoke_controls_{};
  const graph_runtime_services *services_{nullptr};
  const run_state *parent_state_{nullptr};
  detail::runtime_state::invoke_config invoke_config_{};
  detail::runtime_state::invoke_outputs invoke_outputs_{};
  detail::runtime_state::graph_trace_state trace_state_{};
  std::uint64_t run_id_{0U};
  bool skip_state_pre_handlers_{false};
  detail::runtime_state::rerun_state rerun_state_{};
  input_runtime::scratch_buffer scratch_{};
  detail::process_runtime::node_local_process_state_slots
      node_local_process_states_{};
  node_path path_prefix_{};
  graph_process_state *parent_process_state_{nullptr};
  detail::runtime_state::invoke_outputs *nested_outputs_{nullptr};
  detail::runtime_state::invoke_outputs *published_outputs_{nullptr};
  forwarded_checkpoint_map owned_forwarded_checkpoints_{};
  forwarded_checkpoint_map *forwarded_checkpoints_{nullptr};
  std::optional<wh::core::detail::any_resume_scheduler_t> graph_scheduler_{};
  bool retain_inputs_{false};
  std::size_t step_count_{0U};
  std::size_t step_budget_{0U};
  std::unique_ptr<graph_call_options> owned_call_options_{};
  graph_call_scope bound_call_scope_{};
  bool has_component_option_overrides_{false};
  bool emit_debug_events_{false};
  bool collect_transition_log_{false};
  bool emit_state_snapshot_events_{false};
  bool emit_state_delta_events_{false};
  bool emit_runtime_message_events_{false};
  bool emit_custom_events_{false};
  graph_external_interrupt_policy external_interrupt_policy_{};
  graph_external_interrupt_policy_latch external_interrupt_policy_latch_{};
  std::vector<std::uint32_t> deferred_ready_queue_{};
  std::vector<node_path> runtime_node_paths_{};
  std::vector<graph_event_scope> runtime_stream_scopes_{};
  std::vector<wh::core::address> runtime_node_execution_addresses_{};
  std::vector<graph_component_option_map> resolved_component_options_{};
  std::vector<graph_resolved_node_observation> resolved_node_observations_{};
  std::vector<const graph_node_state_handlers *> resolved_state_handlers_{};
  std::size_t current_batch_end_{0U};
  bool external_interrupt_wait_mode_active_{false};
  bool freeze_requested_{false};
  bool freeze_external_{false};
  std::optional<std::chrono::steady_clock::time_point>
      external_interrupt_deadline_{};
};

} // namespace wh::compose::detail::invoke_runtime
