// Defines compose graph invoke/runtime frame and action data types.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wh/compose/graph/detail/runtime/input.hpp"
#include "wh/compose/graph/detail/runtime/process.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/graph/invoke_types.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/error.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::compose {

struct compiled_node;
struct graph_node_state_handlers;

namespace detail::invoke_runtime {
class invoke_session;

template <typename receiver_t, typename derived_t, typename graph_scheduler_t>
class invoke_join_base;

template <typename state_t, typename receiver_t, typename derived_t, typename graph_scheduler_t>
class invoke_stage_run;
} // namespace detail::invoke_runtime

namespace detail::runtime_state {

/// Single invoke-owned binding, output, and scheduling state.
struct invoke_state {
  /// Immutable invoke controls captured at graph entry.
  graph_invoke_controls controls{};
  /// Optional runtime services injected by the host.
  const graph_runtime_services *services{nullptr};
  /// Optional parent runtime when running as a nested graph.
  const detail::invoke_runtime::invoke_session *parent_state{nullptr};
  /// Resolved invoke configuration after services and controls merge.
  invoke_config config{};
  /// Mutable publishable outputs accumulated during the run.
  invoke_outputs outputs{};
  /// Monotonic run id assigned at invoke entry.
  std::uint64_t run_id{0U};
  /// Bound runtime path prefix for this invoke.
  node_path path_prefix{};
  /// Optional parent process-state when invoked as a nested node.
  graph_process_state *parent_process_state{nullptr};
  /// Optional nested output sink for parent aggregation.
  invoke_outputs *nested_outputs{nullptr};
  /// Optional explicit publish target owned by caller.
  invoke_outputs *published_outputs{nullptr};
  /// Owned forwarded checkpoints captured at invoke entry.
  forwarded_checkpoint_map owned_forwarded_checkpoints{};
  /// Active forwarded checkpoint table visible to this run.
  forwarded_checkpoint_map *forwarded_checkpoints{nullptr};
  /// Control scheduler fixed for the whole invoke lifetime.
  std::optional<wh::core::detail::any_resume_scheduler_t> control_scheduler{};
  /// Work scheduler fixed for the whole invoke lifetime.
  std::optional<wh::core::detail::any_resume_scheduler_t> work_scheduler{};
  /// True retains node inputs for retries, checkpoint, or interrupt hooks.
  bool retain_inputs{false};
  /// Monotonic runtime step counter.
  std::size_t step_count{0U};
  /// Effective step budget resolved for this invoke.
  std::size_t step_budget{0U};
  /// Owned call-options storage when invoke took an rvalue options object.
  std::unique_ptr<graph_call_options> owned_call_options{};
  /// Bound call scope used by the whole runtime.
  graph_call_scope bound_call_scope{};
  /// Precomputed start-node branch selection.
  std::optional<std::vector<std::uint32_t>> start_entry_selection{};
  /// True once terminal/checkpoint paths requested one persist stage.
  bool persist_requested{false};
  /// True while one persist stage is currently running.
  bool persist_inflight{false};
};

} // namespace detail::runtime_state

namespace detail::invoke_runtime {

enum class stage : std::uint8_t {
  input = 0U,
  pre_state,
  prepare,
  node,
  post_state,
  freeze,
  persist,
};

struct attempt_id {
  static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();

  std::uint32_t slot{invalid};

  [[nodiscard]] auto has_value() const noexcept -> bool { return slot != invalid; }

  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  friend auto operator==(attempt_id, attempt_id) noexcept -> bool = default;
};

struct attempt_input {
  std::optional<graph_value> payload{};
  std::optional<input_runtime::reader_lowering> lowering{};
};

struct attempt_slot {
  stage current_stage{stage::input};
  std::uint32_t node_id{0U};
  graph_state_cause cause{};
  const compiled_node *node{nullptr};
  const graph_node_state_handlers *state_handlers{nullptr};
  std::size_t retry_budget{0U};
  std::size_t attempt{0U};
  std::optional<std::chrono::milliseconds> timeout_budget{};
  std::optional<attempt_input> input{};
  node_runtime runtime{};
  runtime_state::node_scope node_scope{};
  process_runtime::scoped_node_local_process_state node_local_scope{};
};

struct state_step {
  attempt_id attempt{};
  std::optional<graph_sender> sender{};
};

enum class ready_action_kind : std::uint8_t {
  no_ready,
  continue_scan,
  launch,
  terminal_error,
};

struct ready_action {
  ready_action_kind kind{ready_action_kind::continue_scan};
  attempt_id attempt{};
  wh::core::error_code error{wh::core::errc::ok};

  [[nodiscard]] static auto no_ready() noexcept -> ready_action {
    return ready_action{.kind = ready_action_kind::no_ready};
  }

  [[nodiscard]] static auto continue_scan() noexcept -> ready_action {
    return ready_action{.kind = ready_action_kind::continue_scan};
  }

  [[nodiscard]] static auto terminal_error(const wh::core::error_code code) noexcept
      -> ready_action {
    return ready_action{
        .kind = ready_action_kind::terminal_error,
        .error = code,
    };
  }

  [[nodiscard]] static auto launch(const attempt_id attempt_value) -> ready_action {
    return ready_action{
        .kind = ready_action_kind::launch,
        .attempt = attempt_value,
    };
  }
};

struct pregel_action {
  enum class kind : std::uint8_t {
    waiting = 0U,
    skip,
    launch,
    terminal_error,
  };

  kind action{kind::waiting};
  std::uint32_t node_id{0U};
  graph_state_cause cause{};
  attempt_id attempt{};
  wh::core::error_code error{wh::core::errc::ok};

  [[nodiscard]] static auto waiting(const std::uint32_t node_id_value) noexcept -> pregel_action {
    return pregel_action{
        .action = kind::waiting,
        .node_id = node_id_value,
    };
  }

  [[nodiscard]] static auto skip(const std::uint32_t node_id_value, graph_state_cause cause_value)
      -> pregel_action {
    return pregel_action{
        .action = kind::skip,
        .node_id = node_id_value,
        .cause = std::move(cause_value),
    };
  }

  [[nodiscard]] static auto launch(const std::uint32_t node_id_value, graph_state_cause cause_value,
                                   const attempt_id attempt_value) -> pregel_action {
    return pregel_action{
        .action = kind::launch,
        .node_id = node_id_value,
        .cause = std::move(cause_value),
        .attempt = attempt_value,
    };
  }

  [[nodiscard]] static auto terminal_error(const std::uint32_t node_id_value,
                                           graph_state_cause cause_value,
                                           const wh::core::error_code code) noexcept
      -> pregel_action {
    return pregel_action{
        .action = kind::terminal_error,
        .node_id = node_id_value,
        .cause = std::move(cause_value),
        .error = code,
    };
  }
};

} // namespace detail::invoke_runtime

} // namespace wh::compose
