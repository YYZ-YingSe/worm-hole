// Defines composition helpers for interrupt-aware graph execution,
// including interrupt propagation and state handoff utilities.
#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/compose/types.hpp"
#include "wh/core/any.hpp"
#include "wh/core/function.hpp"
#include "wh/core/resume_state.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

namespace detail {

/// Generates monotonic numeric suffix for auto-created interrupt ids.
[[nodiscard]] inline auto next_interrupt_sequence() noexcept -> std::uint64_t {
  static std::atomic<std::uint64_t> sequence{1U};
  return sequence.fetch_add(1U, std::memory_order_relaxed);
}

} // namespace detail

/// Creates a process-local interrupt id like `interrupt-<n>`.
[[nodiscard]] inline auto make_interrupt_id() -> std::string {
  return std::string{"interrupt-"} +
         std::to_string(detail::next_interrupt_sequence());
}

/// Human decision kind injected before resume.
enum class interrupt_decision_kind : std::uint8_t {
  /// Resume with captured state as approved.
  approve = 0U,
  /// Resume with caller-edited replacement payload.
  edit,
  /// Reject resume and keep interrupt pending.
  reject,
};

/// Audit metadata recorded for one manual decision.
struct interrupt_decision_audit {
  /// Stable audit event id for trace/replay correlation.
  std::string audit_id{};
  /// Operator/agent id that made the decision.
  std::string actor{};
  /// Optional human-readable decision reason.
  std::string reason{};
  /// Decision timestamp.
  std::chrono::system_clock::time_point decided_at{
      std::chrono::system_clock::now()};
};

/// Resume decision payload keyed by interrupt-context id.
struct interrupt_resume_decision {
  /// Target interrupt-context id (batch key semantics).
  std::string interrupt_context_id{};
  /// Decision kind for this interrupt target.
  interrupt_decision_kind decision{interrupt_decision_kind::approve};
  /// Optional edited payload used when `decision == edit`.
  wh::core::any edited_payload{};
  /// Audit metadata persisted alongside decision payload.
  interrupt_decision_audit audit{};
};

/// Node-level interrupt hook used by graph pre/post invoke boundaries.
using graph_interrupt_node_hook = wh::core::callback_function<
    wh::core::result<std::optional<wh::core::interrupt_signal>>(
        std::string_view, const graph_value &, wh::core::run_context &) const>;

/// Session key for pre-node interrupt hook.
inline constexpr std::string_view graph_interrupt_pre_hook_session_key =
    "compose.graph.interrupt.pre_hook";
/// Session key for post-node interrupt hook.
inline constexpr std::string_view graph_interrupt_post_hook_session_key =
    "compose.graph.interrupt.post_hook";
/// Session key for one batch of subgraph interrupt signals.
inline constexpr std::string_view graph_subgraph_interrupt_signals_session_key =
    "compose.graph.interrupt.subgraph_signals";

template <typename interrupt_id_t, typename location_t,
          typename state_t = wh::core::any,
          typename payload_t = wh::core::any>
  requires std::constructible_from<std::string, interrupt_id_t &&> &&
           std::constructible_from<wh::core::address, location_t &&>
/// Builds one interrupt signal with explicit id/location and boxed payloads.
[[nodiscard]] inline auto make_interrupt_signal(interrupt_id_t &&interrupt_id,
                                                location_t &&location,
                                                state_t &&state = {},
                                                payload_t &&layer_payload = {})
    -> wh::core::interrupt_signal {
  using state_value_t = wh::core::remove_cvref_t<state_t>;
  using payload_value_t = wh::core::remove_cvref_t<payload_t>;

  wh::core::any boxed_state;
  if constexpr (std::same_as<state_value_t, wh::core::any>) {
    boxed_state = std::forward<state_t>(state);
  } else {
    boxed_state = wh::core::any{std::in_place_type<state_value_t>,
                                std::forward<state_t>(state)};
  }

  wh::core::any boxed_payload;
  if constexpr (std::same_as<payload_value_t, wh::core::any>) {
    boxed_payload = std::forward<payload_t>(layer_payload);
  } else {
    boxed_payload = wh::core::any{std::in_place_type<payload_value_t>,
                                  std::forward<payload_t>(layer_payload)};
  }

  return wh::core::interrupt_signal{
      std::string{std::forward<interrupt_id_t>(interrupt_id)},
      wh::core::address{std::forward<location_t>(location)},
      std::move(boxed_state),
      std::move(boxed_payload),
      false};
}

template <typename location_t, typename state_t = wh::core::any,
          typename payload_t = wh::core::any>
  requires std::constructible_from<wh::core::address, location_t &&>
/// Builds one interrupt signal with auto-generated id.
[[nodiscard]] inline auto make_interrupt_signal(location_t &&location,
                                                state_t &&state = {},
                                                payload_t &&layer_payload = {})
    -> wh::core::interrupt_signal {
  return make_interrupt_signal(make_interrupt_id(),
                               std::forward<location_t>(location),
                               std::forward<state_t>(state),
                               std::forward<payload_t>(layer_payload));
}

/// Converts signal shape into resume-context shape (copy view).
[[nodiscard]] inline auto
to_interrupt_context(const wh::core::interrupt_signal &signal)
    -> wh::core::interrupt_context {
  return wh::core::to_interrupt_context(signal);
}

/// Builds flat interrupt-id lookup snapshot from signal list (copy path).
[[nodiscard]] inline auto flatten_interrupt_signals(
    const std::span<const wh::core::interrupt_signal> signals)
    -> wh::core::interrupt_snapshot {
  return wh::core::flatten_interrupt_signals(signals);
}

/// Builds flat interrupt-id lookup snapshot from signal list (move path).
[[nodiscard]] inline auto
flatten_interrupt_signals(std::vector<wh::core::interrupt_signal> &&signals)
    -> wh::core::interrupt_snapshot {
  return wh::core::flatten_interrupt_signals(std::move(signals));
}

/// Rebuilds hierarchical signal tree from flat signal list.
[[nodiscard]] inline auto rebuild_interrupt_signal_tree(
    const std::span<const wh::core::interrupt_signal> signals)
    -> std::vector<wh::core::interrupt_signal_tree_node> {
  return wh::core::rebuild_interrupt_signal_tree(signals);
}

/// Rebuilds hierarchical context tree from flat context list.
[[nodiscard]] inline auto rebuild_interrupt_context_tree(
    const std::span<const wh::core::interrupt_context> contexts)
    -> std::vector<wh::core::interrupt_context_tree_node> {
  return wh::core::rebuild_interrupt_context_tree(contexts);
}

/// Converts signal tree representation into context tree representation.
[[nodiscard]] inline auto to_interrupt_context_tree(
    const std::span<const wh::core::interrupt_signal_tree_node> roots)
    -> std::vector<wh::core::interrupt_context_tree_node> {
  return wh::core::to_interrupt_context_tree(roots);
}

/// Converts context tree representation back into signal tree representation.
[[nodiscard]] inline auto to_interrupt_signal_tree(
    const std::span<const wh::core::interrupt_context_tree_node> roots)
    -> std::vector<wh::core::interrupt_signal_tree_node> {
  return wh::core::to_interrupt_signal_tree(roots);
}

/// Flattens hierarchical signal tree into flat signal list.
[[nodiscard]] inline auto flatten_interrupt_signal_tree(
    const std::span<const wh::core::interrupt_signal_tree_node> roots)
    -> std::vector<wh::core::interrupt_signal> {
  return wh::core::flatten_interrupt_signal_tree(roots);
}

/// Filters one interrupt context location by allowed address segments (copy path).
[[nodiscard]] inline auto project_interrupt_context(
    const wh::core::interrupt_context &context,
    const std::span<const std::string_view> allowed_segments)
    -> wh::core::interrupt_context {
  return wh::core::project_interrupt_context(context, allowed_segments);
}

/// Filters one interrupt context location by allowed address segments (move path).
[[nodiscard]] inline auto project_interrupt_context(
    wh::core::interrupt_context &&context,
    const std::span<const std::string_view> allowed_segments)
    -> wh::core::interrupt_context {
  return wh::core::project_interrupt_context(std::move(context),
                                             allowed_segments);
}

/// Converts one interrupt context back into re-interrupt signal shape.
[[nodiscard]] inline auto
to_reinterrupt_signal(const wh::core::interrupt_context &context)
    -> wh::core::interrupt_signal {
  return wh::core::interrupt_signal{
      .interrupt_id = context.interrupt_id,
      .location = context.location,
      .state = wh::core::clone_interrupt_payload_any(context.state),
      .layer_payload = wh::core::clone_interrupt_payload_any(context.layer_payload),
      .used = context.used,
      .parent_locations = context.parent_locations,
      .trigger_reason = context.trigger_reason,
  };
}

/// Converts one movable interrupt context into re-interrupt signal shape.
[[nodiscard]] inline auto
to_reinterrupt_signal(wh::core::interrupt_context &&context)
    -> wh::core::interrupt_signal {
  return wh::core::interrupt_signal{
      .interrupt_id = std::move(context.interrupt_id),
      .location = std::move(context.location),
      .state = std::move(context.state),
      .layer_payload = std::move(context.layer_payload),
      .used = context.used,
      .parent_locations = std::move(context.parent_locations),
      .trigger_reason = std::move(context.trigger_reason),
  };
}

/// Merges root/subgraph interrupt sources into one deduplicated context list.
[[nodiscard]] inline auto merge_interrupt_sources(
    const std::span<const wh::core::interrupt_context> root_contexts,
    const std::span<const wh::core::interrupt_signal> subgraph_signals)
    -> std::vector<wh::core::interrupt_context> {
  std::vector<wh::core::interrupt_context> merged{};
  merged.reserve(root_contexts.size() + subgraph_signals.size());
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      seen_ids{};
  for (const auto &context : root_contexts) {
    if (context.interrupt_id.empty() || !seen_ids.insert(context.interrupt_id).second) {
      continue;
    }
    merged.push_back(context);
  }
  for (const auto &signal : subgraph_signals) {
    if (signal.interrupt_id.empty() || !seen_ids.insert(signal.interrupt_id).second) {
      continue;
    }
    merged.push_back(wh::core::to_interrupt_context(signal));
  }
  return merged;
}

namespace detail {

template <typename contexts_t>
  requires std::same_as<wh::core::remove_cvref_t<contexts_t>,
                        std::vector<wh::core::interrupt_context>>
[[nodiscard]] inline auto fallback_to_single_interrupt_when_empty_impl(
    contexts_t &&contexts, const wh::core::address &location,
    wh::core::any state)
    -> std::vector<wh::core::interrupt_context> {
  std::vector<wh::core::interrupt_context> owned{};
  if constexpr (std::is_lvalue_reference_v<contexts_t>) {
    owned = contexts;
  } else {
    owned = std::move(contexts);
  }
  if (!owned.empty()) {
    return owned;
  }
  owned.push_back(wh::core::interrupt_context{
      .interrupt_id = make_interrupt_id(),
      .location = location,
      .state = std::move(state),
      .layer_payload = wh::core::any{},
      .used = false,
  });
  return owned;
}

} // namespace detail

/// Falls back to single-point interrupt when child list is empty (copy path).
[[nodiscard]] inline auto fallback_to_single_interrupt_when_empty(
    const std::vector<wh::core::interrupt_context> &contexts,
    const wh::core::address &location, wh::core::any state = {})
    -> std::vector<wh::core::interrupt_context> {
  return detail::fallback_to_single_interrupt_when_empty_impl(contexts, location,
                                                              std::move(state));
}

/// Falls back to single-point interrupt when child list is empty (move path).
[[nodiscard]] inline auto fallback_to_single_interrupt_when_empty(
    std::vector<wh::core::interrupt_context> &&contexts,
    const wh::core::address &location, wh::core::any state = {})
    -> std::vector<wh::core::interrupt_context> {
  return detail::fallback_to_single_interrupt_when_empty_impl(
      std::move(contexts), location, std::move(state));
}

} // namespace wh::compose
