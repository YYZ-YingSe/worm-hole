// Defines the public typed compose invoke request/report surface.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/error.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::compose {

/// Host-injected runtime services visible to one graph invoke.
/// These services are invoke-borrowed host handles rather than persisted value
/// state and must outlive the active invoke.
struct graph_runtime_services {
  /// Checkpoint-related host capabilities used by restore/persist paths.
  struct checkpoint_services {
    /// In-memory checkpoint store used by restore/save when present.
    checkpoint_store *store{nullptr};
    /// Pluggable checkpoint backend used by restore/save when present.
    checkpoint_backend *backend{nullptr};
    /// Stream conversion registry used by checkpoint save/load.
    const checkpoint_stream_codecs *stream_codecs{nullptr};
    /// Serializer pair used by checkpoint encode/decode.
    const checkpoint_serializer *serializer{nullptr};
  } checkpoint{};

  /// Node state-handler registry used by runtime pre/post hooks.
  const graph_state_handler_registry *state_handlers{nullptr};
};

/// Typed per-invoke controls that should not be encoded through session keys.
struct graph_invoke_controls {
  /// Existing graph call options kept as the top-level user call contract.
  graph_call_options call{};

  /// Checkpoint load/save controls for this invoke.
  struct checkpoint_controls {
    /// Optional restore/load options for this invoke.
    std::optional<checkpoint_load_options> load{};
    /// Optional persist/save options for this invoke.
    std::optional<checkpoint_save_options> save{};
    /// Optional whole-checkpoint modifier applied before restore.
    checkpoint_state_modifier before_load{nullptr};
    /// Optional NodePath-scoped node-state modifiers applied before restore.
    checkpoint_node_hooks before_load_nodes{};
    /// Optional whole-checkpoint modifier applied after restore.
    checkpoint_state_modifier after_load{nullptr};
    /// Optional NodePath-scoped node-state modifiers applied after restore.
    checkpoint_node_hooks after_load_nodes{};
    /// Optional whole-checkpoint modifier applied before persist.
    checkpoint_state_modifier before_save{nullptr};
    /// Optional NodePath-scoped node-state modifiers applied before persist.
    checkpoint_node_hooks before_save_nodes{};
    /// Optional whole-checkpoint modifier applied after persist.
    checkpoint_state_modifier after_save{nullptr};
    /// Optional NodePath-scoped node-state modifiers applied after persist.
    checkpoint_node_hooks after_save_nodes{};
    /// Optional one-shot forwarded checkpoints consumed by nested restore.
    forwarded_checkpoint_map forwarded_once{};
  } checkpoint{};

  /// Resume patch controls for this invoke.
  struct resume_controls {
    /// Optional single audited resume decision.
    std::optional<interrupt_resume_decision> decision{};
    /// Optional batch resume payloads keyed by interrupt-context id.
    std::vector<resume_batch_item> batch_items{};
    /// Optional interrupt contexts participating in this resume flow.
    std::vector<wh::core::interrupt_context> contexts{};
    /// True re-interrupts unmatched contexts after explicit resume patching.
    bool reinterrupt_unmatched{true};
  } resume{};

  /// Scheduler and branch policy controls for this invoke.
  struct schedule_controls {
    /// Optional invoke-local Pregel max-step override.
    std::optional<std::size_t> pregel_max_steps{};
    /// Optional invoke-local branch merge strategy override.
    std::optional<graph_branch_merge> branch_merge{};
  } schedule{};

  /// Interrupt hook controls for this invoke.
  struct interrupt_controls {
    /// Optional pre-node interrupt hook.
    graph_interrupt_node_hook pre_hook{nullptr};
    /// Optional post-node interrupt hook.
    graph_interrupt_node_hook post_hook{nullptr};
    /// Optional subgraph interrupt signals forwarded into this invoke.
    std::vector<wh::core::interrupt_signal> subgraph_signals{};
  } interrupt{};
};

/// Explicit invoke scheduler pair resolved at graph entry.
struct graph_invoke_schedulers {
  /// Control-plane scheduler that owns resume / frontier / settle.
  std::optional<wh::core::detail::any_resume_scheduler_t> control_scheduler{};
  /// Work-plane scheduler for node work and runtime-owned async lowering.
  std::optional<wh::core::detail::any_resume_scheduler_t> work_scheduler{};

  template <typename scheduler_t>
  auto set_control_scheduler(scheduler_t scheduler)
      -> graph_invoke_schedulers & {
    control_scheduler =
        wh::core::detail::erase_resume_scheduler(std::move(scheduler));
    return *this;
  }

  template <typename scheduler_t>
  auto set_work_scheduler(scheduler_t scheduler) -> graph_invoke_schedulers & {
    work_scheduler =
        wh::core::detail::erase_resume_scheduler(std::move(scheduler));
    return *this;
  }
};

/// Structured runtime report returned after one graph invoke completes.
struct graph_run_report {
  /// Transition log emitted during this invoke.
  graph_transition_log transition_log{};
  /// Terminal-path completed node set.
  std::vector<std::string> completed_node_keys{};
  /// Debug scheduling events emitted during this invoke.
  std::vector<graph_debug_stream_event> debug_events{};
  /// State snapshot events emitted during this invoke.
  std::vector<graph_state_snapshot_event> state_snapshot_events{};
  /// State delta events emitted during this invoke.
  std::vector<graph_state_delta_event> state_delta_events{};
  /// Runtime message events emitted during this invoke.
  std::vector<graph_runtime_message_event> runtime_message_events{};
  /// Custom channel events emitted during this invoke.
  std::vector<graph_custom_event> custom_events{};
  /// Remaining forwarded checkpoint keys after nested restore consumption.
  std::vector<std::string> remaining_forwarded_checkpoint_keys{};
  /// Optional step-limit error detail captured during this invoke.
  std::optional<graph_step_limit_error_detail> step_limit_error{};
  /// Optional node-timeout detail captured during this invoke.
  std::optional<graph_node_timeout_error_detail> node_timeout_error{};
  /// Optional node-run detail captured during this invoke.
  std::optional<graph_node_run_error_detail> node_run_error{};
  /// Optional graph-run detail captured during this invoke.
  std::optional<graph_run_error_detail> graph_run_error{};
  /// Optional stream-read error detail captured during this invoke.
  std::optional<graph_new_stream_read_error_detail> stream_read_error{};
  /// Optional external interrupt resolution captured during this invoke.
  std::optional<graph_external_interrupt_resolution_kind>
      interrupt_resolution{};
  /// Optional checkpoint error detail captured during this invoke.
  std::optional<checkpoint_error_detail> checkpoint_error{};
};

/// Public graph input source kind.
enum class graph_input_kind : std::uint8_t {
  value = 0U,
  stream,
  restore_checkpoint,
};

/// Explicit graph invoke input declared against the public graph boundary.
class graph_input {
public:
  graph_input() = default;

  template <typename value_t>
    requires(!std::same_as<std::remove_cvref_t<value_t>, graph_input> &&
             std::constructible_from<graph_value, value_t &&>)
  [[nodiscard]] static auto value(value_t &&value) -> graph_input {
    return graph_input{graph_input_kind::value,
                       graph_value{std::forward<value_t>(value)}};
  }

  [[nodiscard]] static auto stream(graph_stream_reader reader) -> graph_input {
    return graph_input{graph_input_kind::stream,
                       graph_value{std::move(reader)}};
  }

  [[nodiscard]] static auto restore_checkpoint() -> graph_input {
    return graph_input{graph_input_kind::restore_checkpoint, {}};
  }

  [[nodiscard]] auto kind() const noexcept -> graph_input_kind {
    return kind_;
  }

  [[nodiscard]] auto value_payload() noexcept -> graph_value * {
    return kind_ == graph_input_kind::value ? std::addressof(payload_) : nullptr;
  }

  [[nodiscard]] auto value_payload() const noexcept -> const graph_value * {
    return kind_ == graph_input_kind::value ? std::addressof(payload_)
                                            : nullptr;
  }

  [[nodiscard]] auto stream_payload() noexcept -> graph_stream_reader * {
    if (kind_ != graph_input_kind::stream) {
      return nullptr;
    }
    return wh::core::any_cast<graph_stream_reader>(&payload_);
  }

  [[nodiscard]] auto stream_payload() const noexcept
      -> const graph_stream_reader * {
    if (kind_ != graph_input_kind::stream) {
      return nullptr;
    }
    return wh::core::any_cast<graph_stream_reader>(&payload_);
  }

  [[nodiscard]] auto into_payload() && -> graph_value {
    return std::move(payload_);
  }

private:
  graph_input(graph_input_kind kind, graph_value payload)
      : payload_(std::move(payload)), kind_(kind) {}

  graph_value payload_{};
  graph_input_kind kind_{graph_input_kind::value};
};

/// One public typed graph invoke request.
struct graph_invoke_request {
  /// Graph input payload for this invoke.
  graph_input input{};
  /// Explicit invoke controls for this invoke.
  graph_invoke_controls controls{};
  /// Optional invoke-borrowed host runtime services visible to this invoke.
  const graph_runtime_services *services{nullptr};
};

/// One public typed graph invoke result that preserves both status and report.
struct graph_invoke_result {
  /// Output status produced by this invoke.
  wh::core::result<graph_value> output_status{};
  /// Structured runtime report collected for this invoke.
  graph_run_report report{};
};

} // namespace wh::compose

namespace wh::compose::detail {

[[nodiscard]] inline auto
into_owned_forwarded_checkpoint_map(const wh::compose::forwarded_checkpoint_map &value)
    -> wh::core::result<wh::compose::forwarded_checkpoint_map> {
  wh::compose::forwarded_checkpoint_map owned{};
  owned.reserve(value.size());
  for (const auto &[key, checkpoint] : value) {
    auto owned_checkpoint = wh::core::into_owned(checkpoint);
    if (owned_checkpoint.has_error()) {
      return wh::core::result<wh::compose::forwarded_checkpoint_map>::failure(
          owned_checkpoint.error());
    }
    owned.insert_or_assign(key, std::move(owned_checkpoint).value());
  }
  return owned;
}

[[nodiscard]] inline auto
into_owned_forwarded_checkpoint_map(wh::compose::forwarded_checkpoint_map &&value)
    -> wh::core::result<wh::compose::forwarded_checkpoint_map> {
  wh::compose::forwarded_checkpoint_map owned{};
  owned.reserve(value.size());
  for (auto iter = value.begin(); iter != value.end();) {
    auto node = value.extract(iter++);
    auto owned_checkpoint = wh::core::into_owned(std::move(node.mapped()));
    if (owned_checkpoint.has_error()) {
      return wh::core::result<wh::compose::forwarded_checkpoint_map>::failure(
          owned_checkpoint.error());
    }
    node.mapped() = std::move(owned_checkpoint).value();
    owned.insert(std::move(node));
  }
  return owned;
}

[[nodiscard]] inline auto
into_owned_graph_input(const wh::compose::graph_input &value)
    -> wh::core::result<wh::compose::graph_input> {
  if (value.kind() == wh::compose::graph_input_kind::restore_checkpoint) {
    return wh::compose::graph_input::restore_checkpoint();
  }
  if (value.kind() == wh::compose::graph_input_kind::value) {
    auto input = wh::core::into_owned(*value.value_payload());
    if (input.has_error()) {
      return wh::core::result<wh::compose::graph_input>::failure(input.error());
    }
    return wh::compose::graph_input::value(std::move(input).value());
  }
  auto stream = wh::core::into_owned(*value.stream_payload());
  if (stream.has_error()) {
    return wh::core::result<wh::compose::graph_input>::failure(stream.error());
  }
  return wh::compose::graph_input::stream(std::move(stream).value());
}

[[nodiscard]] inline auto
into_owned_graph_input(wh::compose::graph_input &&value)
    -> wh::core::result<wh::compose::graph_input> {
  if (value.kind() == wh::compose::graph_input_kind::restore_checkpoint) {
    return wh::compose::graph_input::restore_checkpoint();
  }
  if (value.kind() == wh::compose::graph_input_kind::value) {
    auto input = wh::core::into_owned(std::move(*value.value_payload()));
    if (input.has_error()) {
      return wh::core::result<wh::compose::graph_input>::failure(
          input.error());
    }
    return wh::compose::graph_input::value(std::move(input).value());
  }
  auto stream = wh::core::into_owned(std::move(*value.stream_payload()));
  if (stream.has_error()) {
    return wh::core::result<wh::compose::graph_input>::failure(stream.error());
  }
  return wh::compose::graph_input::stream(std::move(stream).value());
}

[[nodiscard]] inline auto
into_owned_graph_invoke_controls(const wh::compose::graph_invoke_controls &value)
    -> wh::core::result<wh::compose::graph_invoke_controls> {
  auto call = wh::core::into_owned(value.call);
  if (call.has_error()) {
    return wh::core::result<wh::compose::graph_invoke_controls>::failure(call.error());
  }
  auto forwarded_once = into_owned_forwarded_checkpoint_map(value.checkpoint.forwarded_once);
  if (forwarded_once.has_error()) {
    return wh::core::result<wh::compose::graph_invoke_controls>::failure(
        forwarded_once.error());
  }

  std::optional<wh::compose::interrupt_resume_decision> decision{};
  if (value.resume.decision.has_value()) {
    auto owned_decision = wh::core::into_owned(*value.resume.decision);
    if (owned_decision.has_error()) {
      return wh::core::result<wh::compose::graph_invoke_controls>::failure(
          owned_decision.error());
    }
    decision = std::move(owned_decision).value();
  }

  std::vector<wh::compose::resume_batch_item> batch_items{};
  batch_items.reserve(value.resume.batch_items.size());
  for (const auto &item : value.resume.batch_items) {
    auto owned_item = wh::core::into_owned(item);
    if (owned_item.has_error()) {
      return wh::core::result<wh::compose::graph_invoke_controls>::failure(
          owned_item.error());
    }
    batch_items.push_back(std::move(owned_item).value());
  }

  std::vector<wh::core::interrupt_context> contexts{};
  contexts.reserve(value.resume.contexts.size());
  for (const auto &context : value.resume.contexts) {
    auto owned_context = wh::core::into_owned(context);
    if (owned_context.has_error()) {
      return wh::core::result<wh::compose::graph_invoke_controls>::failure(
          owned_context.error());
    }
    contexts.push_back(std::move(owned_context).value());
  }

  std::vector<wh::core::interrupt_signal> subgraph_signals{};
  subgraph_signals.reserve(value.interrupt.subgraph_signals.size());
  for (const auto &signal : value.interrupt.subgraph_signals) {
    auto owned_signal = wh::core::into_owned(signal);
    if (owned_signal.has_error()) {
      return wh::core::result<wh::compose::graph_invoke_controls>::failure(
          owned_signal.error());
    }
    subgraph_signals.push_back(std::move(owned_signal).value());
  }

  return wh::compose::graph_invoke_controls{
      .call = std::move(call).value(),
      .checkpoint =
          wh::compose::graph_invoke_controls::checkpoint_controls{
              .load = value.checkpoint.load,
              .save = value.checkpoint.save,
              .before_load = value.checkpoint.before_load,
              .before_load_nodes = value.checkpoint.before_load_nodes,
              .after_load = value.checkpoint.after_load,
              .after_load_nodes = value.checkpoint.after_load_nodes,
              .before_save = value.checkpoint.before_save,
              .before_save_nodes = value.checkpoint.before_save_nodes,
              .after_save = value.checkpoint.after_save,
              .after_save_nodes = value.checkpoint.after_save_nodes,
              .forwarded_once = std::move(forwarded_once).value(),
          },
      .resume =
          wh::compose::graph_invoke_controls::resume_controls{
              .decision = std::move(decision),
              .batch_items = std::move(batch_items),
              .contexts = std::move(contexts),
              .reinterrupt_unmatched = value.resume.reinterrupt_unmatched,
          },
      .schedule = value.schedule,
      .interrupt =
          wh::compose::graph_invoke_controls::interrupt_controls{
              .pre_hook = value.interrupt.pre_hook,
              .post_hook = value.interrupt.post_hook,
              .subgraph_signals = std::move(subgraph_signals),
          },
  };
}

[[nodiscard]] inline auto
into_owned_graph_invoke_controls(wh::compose::graph_invoke_controls &&value)
    -> wh::core::result<wh::compose::graph_invoke_controls> {
  auto call = wh::core::into_owned(std::move(value.call));
  if (call.has_error()) {
    return wh::core::result<wh::compose::graph_invoke_controls>::failure(call.error());
  }
  auto forwarded_once =
      into_owned_forwarded_checkpoint_map(std::move(value.checkpoint.forwarded_once));
  if (forwarded_once.has_error()) {
    return wh::core::result<wh::compose::graph_invoke_controls>::failure(
        forwarded_once.error());
  }

  std::optional<wh::compose::interrupt_resume_decision> decision{};
  if (value.resume.decision.has_value()) {
    auto owned_decision = wh::core::into_owned(std::move(*value.resume.decision));
    if (owned_decision.has_error()) {
      return wh::core::result<wh::compose::graph_invoke_controls>::failure(
          owned_decision.error());
    }
    decision = std::move(owned_decision).value();
  }

  std::vector<wh::compose::resume_batch_item> batch_items{};
  batch_items.reserve(value.resume.batch_items.size());
  for (auto &item : value.resume.batch_items) {
    auto owned_item = wh::core::into_owned(std::move(item));
    if (owned_item.has_error()) {
      return wh::core::result<wh::compose::graph_invoke_controls>::failure(
          owned_item.error());
    }
    batch_items.push_back(std::move(owned_item).value());
  }

  std::vector<wh::core::interrupt_context> contexts{};
  contexts.reserve(value.resume.contexts.size());
  for (auto &context : value.resume.contexts) {
    auto owned_context = wh::core::into_owned(std::move(context));
    if (owned_context.has_error()) {
      return wh::core::result<wh::compose::graph_invoke_controls>::failure(
          owned_context.error());
    }
    contexts.push_back(std::move(owned_context).value());
  }

  std::vector<wh::core::interrupt_signal> subgraph_signals{};
  subgraph_signals.reserve(value.interrupt.subgraph_signals.size());
  for (auto &signal : value.interrupt.subgraph_signals) {
    auto owned_signal = wh::core::into_owned(std::move(signal));
    if (owned_signal.has_error()) {
      return wh::core::result<wh::compose::graph_invoke_controls>::failure(
          owned_signal.error());
    }
    subgraph_signals.push_back(std::move(owned_signal).value());
  }

  return wh::compose::graph_invoke_controls{
      .call = std::move(call).value(),
      .checkpoint =
          wh::compose::graph_invoke_controls::checkpoint_controls{
              .load = std::move(value.checkpoint.load),
              .save = std::move(value.checkpoint.save),
              .before_load = std::move(value.checkpoint.before_load),
              .before_load_nodes = std::move(value.checkpoint.before_load_nodes),
              .after_load = std::move(value.checkpoint.after_load),
              .after_load_nodes = std::move(value.checkpoint.after_load_nodes),
              .before_save = std::move(value.checkpoint.before_save),
              .before_save_nodes = std::move(value.checkpoint.before_save_nodes),
              .after_save = std::move(value.checkpoint.after_save),
              .after_save_nodes = std::move(value.checkpoint.after_save_nodes),
              .forwarded_once = std::move(forwarded_once).value(),
          },
      .resume =
          wh::compose::graph_invoke_controls::resume_controls{
              .decision = std::move(decision),
              .batch_items = std::move(batch_items),
              .contexts = std::move(contexts),
              .reinterrupt_unmatched = value.resume.reinterrupt_unmatched,
          },
      .schedule = std::move(value.schedule),
      .interrupt =
          wh::compose::graph_invoke_controls::interrupt_controls{
              .pre_hook = std::move(value.interrupt.pre_hook),
              .post_hook = std::move(value.interrupt.post_hook),
              .subgraph_signals = std::move(subgraph_signals),
          },
  };
}

[[nodiscard]] inline auto
into_owned_graph_invoke_request(const wh::compose::graph_invoke_request &value)
    -> wh::core::result<wh::compose::graph_invoke_request> {
  auto input = wh::core::into_owned(value.input);
  if (input.has_error()) {
    return wh::core::result<wh::compose::graph_invoke_request>::failure(input.error());
  }
  auto controls = wh::core::into_owned(value.controls);
  if (controls.has_error()) {
    return wh::core::result<wh::compose::graph_invoke_request>::failure(controls.error());
  }
  return wh::compose::graph_invoke_request{
      .input = std::move(input).value(),
      .controls = std::move(controls).value(),
      .services = value.services,
  };
}

[[nodiscard]] inline auto
into_owned_graph_invoke_request(wh::compose::graph_invoke_request &&value)
    -> wh::core::result<wh::compose::graph_invoke_request> {
  auto input = wh::core::into_owned(std::move(value.input));
  if (input.has_error()) {
    return wh::core::result<wh::compose::graph_invoke_request>::failure(input.error());
  }
  auto controls = wh::core::into_owned(std::move(value.controls));
  if (controls.has_error()) {
    return wh::core::result<wh::compose::graph_invoke_request>::failure(controls.error());
  }
  return wh::compose::graph_invoke_request{
      .input = std::move(input).value(),
      .controls = std::move(controls).value(),
      .services = value.services,
  };
}

[[nodiscard]] inline auto normalize_graph_input(
    const wh::compose::node_contract expected,
    const bool has_restore_source,
    wh::compose::graph_input input) -> wh::core::result<wh::compose::graph_value> {
  if (input.kind() == wh::compose::graph_input_kind::restore_checkpoint) {
    if (!has_restore_source) {
      return wh::core::result<wh::compose::graph_value>::failure(
          wh::core::errc::invalid_argument);
    }
    return wh::core::any(std::monostate{});
  }

  if (input.kind() == wh::compose::graph_input_kind::value) {
    if (expected != wh::compose::node_contract::value) {
      return wh::core::result<wh::compose::graph_value>::failure(
          wh::core::errc::contract_violation);
    }
    auto payload = std::move(input).into_payload();
    if (has_restore_source &&
        wh::core::any_cast<std::monostate>(&payload) != nullptr) {
      return wh::core::result<wh::compose::graph_value>::failure(
          wh::core::errc::contract_violation);
    }
    auto valid = wh::compose::detail::validate_value_boundary_payload(payload);
    if (valid.has_error()) {
      return wh::core::result<wh::compose::graph_value>::failure(valid.error());
    }
    return payload;
  }

  if (expected != wh::compose::node_contract::stream) {
    return wh::core::result<wh::compose::graph_value>::failure(
        wh::core::errc::contract_violation);
  }
  auto payload = std::move(input).into_payload();
  if (wh::core::any_cast<wh::compose::graph_stream_reader>(&payload) ==
      nullptr) {
    return wh::core::result<wh::compose::graph_value>::failure(
        wh::core::errc::type_mismatch);
  }
  return payload;
}

} // namespace wh::compose::detail

namespace wh::core {

template <> struct any_owned_traits<wh::compose::graph_input> {
  [[nodiscard]] static auto into_owned(const wh::compose::graph_input &value)
      -> wh::core::result<wh::compose::graph_input> {
    return wh::compose::detail::into_owned_graph_input(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::graph_input &&value)
      -> wh::core::result<wh::compose::graph_input> {
    return wh::compose::detail::into_owned_graph_input(std::move(value));
  }
};

template <> struct any_owned_traits<wh::compose::graph_invoke_controls> {
  [[nodiscard]] static auto into_owned(const wh::compose::graph_invoke_controls &value)
      -> wh::core::result<wh::compose::graph_invoke_controls> {
    return wh::compose::detail::into_owned_graph_invoke_controls(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::graph_invoke_controls &&value)
      -> wh::core::result<wh::compose::graph_invoke_controls> {
    return wh::compose::detail::into_owned_graph_invoke_controls(std::move(value));
  }
};

template <> struct any_owned_traits<wh::compose::graph_invoke_request> {
  [[nodiscard]] static auto into_owned(const wh::compose::graph_invoke_request &value)
      -> wh::core::result<wh::compose::graph_invoke_request> {
    return wh::compose::detail::into_owned_graph_invoke_request(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::graph_invoke_request &&value)
      -> wh::core::result<wh::compose::graph_invoke_request> {
    return wh::compose::detail::into_owned_graph_invoke_request(std::move(value));
  }
};

} // namespace wh::core
