// Defines resume-time composition utilities that rebuild executable paths
// from persisted checkpoint and interrupt state.
#pragma once

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/compose/runtime/interrupt.hpp"
#include "wh/core/any.hpp"
#include "wh/core/resume_state.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

/// Merges delta resume snapshot into target (copy path).
inline auto merge_resume_state(wh::core::resume_state &target,
                               const wh::core::resume_state &delta)
    -> wh::core::result<void> {
  return target.merge(delta);
}

/// Merges delta resume snapshot into target (move path).
inline auto merge_resume_state(wh::core::resume_state &target,
                               wh::core::resume_state &&delta)
    -> wh::core::result<void> {
  return target.merge(std::move(delta));
}

/// Inserts or replaces one resumable target with pre-boxed payload.
template <typename interrupt_id_t, typename location_t, typename data_t>
  requires std::constructible_from<std::string, interrupt_id_t &&> &&
           std::constructible_from<wh::core::address, location_t &&> &&
           std::same_as<wh::core::remove_cvref_t<data_t>, wh::core::any>
inline auto add_resume_target(wh::core::resume_state &state,
                              interrupt_id_t &&interrupt_id,
                              location_t &&location, data_t &&data)
    -> wh::core::result<void> {
  return state.upsert(std::forward<interrupt_id_t>(interrupt_id),
                      std::forward<location_t>(location),
                      std::forward<data_t>(data));
}

/// Inserts or replaces one resumable target with typed payload.
template <typename interrupt_id_t, typename location_t, typename value_t>
  requires std::constructible_from<std::string, interrupt_id_t &&> &&
           std::constructible_from<wh::core::address, location_t &&> &&
           (!std::same_as<wh::core::remove_cvref_t<value_t>, wh::core::any>)
inline auto add_resume_target(wh::core::resume_state &state,
                              interrupt_id_t &&interrupt_id,
                              location_t &&location, value_t &&data)
    -> wh::core::result<void> {
  return state.upsert(std::forward<interrupt_id_t>(interrupt_id),
                      std::forward<location_t>(location),
                      std::forward<value_t>(data));
}

/// Consumes typed resume payload by interrupt id and marks entry as used.
template <typename value_t>
[[nodiscard]] inline auto
consume_resume_data(wh::core::resume_state &state,
                    const std::string_view interrupt_id)
    -> wh::core::result<value_t> {
  return state.consume<value_t>(interrupt_id);
}

/// Returns immediate child segments that can be resumed under `location`.
[[nodiscard]] inline auto
next_resume_points(const wh::core::resume_state &state,
                   const wh::core::address &location)
    -> std::vector<std::string> {
  return state.next_resume_points(location);
}

/// Collects interrupt ids inside one address subtree.
[[nodiscard]] inline auto collect_resume_subtree_ids(
    const wh::core::resume_state &state, const wh::core::address &location,
    const wh::core::resume_subtree_query_options options =
        wh::core::resume_subtree_query_options{}) -> std::vector<std::string> {
  return state.collect_subtree_interrupt_ids(location, options);
}

/// Marks all entries in one address subtree as used.
inline auto mark_resume_subtree_used(wh::core::resume_state &state,
                                     const wh::core::address &location)
    -> std::size_t {
  return state.mark_subtree_used(location);
}

/// Erases entries in one address subtree.
inline auto erase_resume_subtree(
    wh::core::resume_state &state, const wh::core::address &location,
    const wh::core::resume_subtree_erase_options options =
        wh::core::resume_subtree_erase_options{}) -> std::size_t {
  return state.erase_subtree(location, options);
}

/// Stored resume payload augmented with manual decision/audit metadata.
struct resume_patch {
  /// Decision action selected by operator or policy.
  interrupt_decision_kind decision{interrupt_decision_kind::approve};
  /// Resume payload consumed by resumed node.
  wh::core::any data{};
  /// Decision audit metadata persisted for replay.
  interrupt_decision_audit audit{};
};

/// One batch resume item keyed by interrupt-context id.
struct resume_batch_item {
  /// Interrupt-context id (batch key semantics).
  std::string interrupt_context_id{};
  /// Resume payload to inject for this context id.
  wh::core::any data{};
};

/// Resume target matching mode for explainable restore decisions.
enum class resume_target_match_kind : std::uint8_t {
  /// Current address is not in active resume subtree.
  none = 0U,
  /// Current address exactly matches one active resume target.
  exact,
  /// Current address is ancestor of one active resume target.
  descendant,
};

/// Explainable target match outcome used by restore orchestrators.
struct resume_target_match_result {
  /// True when current run is in resume flow.
  bool in_resume_flow{false};
  /// Match kind for current address.
  resume_target_match_kind match_kind{resume_target_match_kind::none};
  /// True means caller should re-interrupt immediately.
  bool should_reinterrupt{false};
};

/// Classifies resume target matching for current execution address.
[[nodiscard]] inline auto
classify_resume_target_match(const wh::core::resume_state &state,
                             const wh::core::address &location)
    -> resume_target_match_result {
  const auto in_resume_flow = !state.empty();
  if (!in_resume_flow) {
    return resume_target_match_result{};
  }
  if (state.is_exact_resume_target(location)) {
    return resume_target_match_result{
        .in_resume_flow = true,
        .match_kind = resume_target_match_kind::exact,
        .should_reinterrupt = false,
    };
  }
  if (state.is_resume_target(location)) {
    return resume_target_match_result{
        .in_resume_flow = true,
        .match_kind = resume_target_match_kind::descendant,
        .should_reinterrupt = false,
    };
  }
  return resume_target_match_result{
      .in_resume_flow = true,
      .match_kind = resume_target_match_kind::none,
      .should_reinterrupt = true,
  };
}

/// Applies one manual decision and injects audited resume payload.
[[nodiscard]] inline auto apply_resume_decision(
    wh::core::resume_state &state, const wh::core::interrupt_context &context,
    const interrupt_resume_decision &decision) -> wh::core::result<void> {
  if (decision.interrupt_context_id.empty() ||
      decision.interrupt_context_id != context.interrupt_id) {
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }
  if (decision.decision == interrupt_decision_kind::reject) {
    return wh::core::result<void>::failure(wh::core::errc::canceled);
  }

  resume_patch payload{};
  payload.decision = decision.decision;
  payload.audit = decision.audit;
  wh::core::result<wh::core::any> owned_data{};
  if (decision.decision == interrupt_decision_kind::edit) {
    if (!decision.edited_payload.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    owned_data = wh::core::into_owned(decision.edited_payload);
  } else {
    owned_data = wh::core::into_owned(context.state);
  }
  if (owned_data.has_error()) {
    return wh::core::result<void>::failure(owned_data.error());
  }
  payload.data = std::move(owned_data).value();
  return state.upsert(decision.interrupt_context_id, context.location,
                      std::move(payload));
}

/// Batch injects resume payloads keyed by interrupt-context id.
[[nodiscard]] inline auto
apply_resume_batch(wh::core::resume_state &state,
                   const std::span<const wh::core::interrupt_context> contexts,
                   const std::span<const resume_batch_item> items,
                   const interrupt_decision_audit &audit = {})
    -> wh::core::result<void> {
  std::unordered_map<std::string_view, const wh::core::interrupt_context *>
      context_index{};
  context_index.reserve(contexts.size());
  for (const auto &context : contexts) {
    context_index.insert_or_assign(context.interrupt_id,
                                   std::addressof(context));
  }

  for (const auto &item : items) {
    const auto context_iter = context_index.find(item.interrupt_context_id);
    if (context_iter == context_index.end()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    resume_patch payload{};
    payload.decision = interrupt_decision_kind::edit;
    auto owned_data = wh::core::into_owned(item.data);
    if (owned_data.has_error()) {
      return wh::core::result<void>::failure(owned_data.error());
    }
    payload.data = std::move(owned_data).value();
    payload.audit = audit;
    auto upserted =
        state.upsert(item.interrupt_context_id, context_iter->second->location,
                     std::move(payload));
    if (upserted.has_error()) {
      return upserted;
    }
  }
  return {};
}

/// Collects unmatched contexts and converts them to immediate re-interrupt
/// signals.
[[nodiscard]] inline auto collect_reinterrupts(
    const wh::core::resume_state &state,
    const std::span<const wh::core::interrupt_context> contexts)
    -> wh::core::result<std::vector<wh::core::interrupt_signal>> {
  std::vector<wh::core::interrupt_signal> reinterrupts{};
  reinterrupts.reserve(contexts.size());
  for (const auto &context : contexts) {
    if (!state.contains_interrupt_id(context.interrupt_id) ||
        state.is_used(context.interrupt_id)) {
      auto signal = to_reinterrupt_signal(context);
      if (signal.has_error()) {
        return wh::core::result<std::vector<wh::core::interrupt_signal>>::failure(
            signal.error());
      }
      reinterrupts.push_back(std::move(signal).value());
    }
  }
  return reinterrupts;
}

} // namespace wh::compose

namespace wh::compose::detail {

[[nodiscard]] inline auto into_owned_resume_patch(const wh::compose::resume_patch &value)
    -> wh::core::result<wh::compose::resume_patch> {
  auto data = wh::core::into_owned(value.data);
  if (data.has_error()) {
    return wh::core::result<wh::compose::resume_patch>::failure(data.error());
  }
  return wh::compose::resume_patch{
      .decision = value.decision,
      .data = std::move(data).value(),
      .audit = value.audit,
  };
}

[[nodiscard]] inline auto into_owned_resume_patch(wh::compose::resume_patch &&value)
    -> wh::core::result<wh::compose::resume_patch> {
  auto data = wh::core::into_owned(std::move(value.data));
  if (data.has_error()) {
    return wh::core::result<wh::compose::resume_patch>::failure(data.error());
  }
  return wh::compose::resume_patch{
      .decision = value.decision,
      .data = std::move(data).value(),
      .audit = std::move(value.audit),
  };
}

[[nodiscard]] inline auto into_owned_resume_batch_item(
    const wh::compose::resume_batch_item &value)
    -> wh::core::result<wh::compose::resume_batch_item> {
  auto data = wh::core::into_owned(value.data);
  if (data.has_error()) {
    return wh::core::result<wh::compose::resume_batch_item>::failure(data.error());
  }
  return wh::compose::resume_batch_item{
      .interrupt_context_id = value.interrupt_context_id,
      .data = std::move(data).value(),
  };
}

[[nodiscard]] inline auto into_owned_resume_batch_item(wh::compose::resume_batch_item &&value)
    -> wh::core::result<wh::compose::resume_batch_item> {
  auto data = wh::core::into_owned(std::move(value.data));
  if (data.has_error()) {
    return wh::core::result<wh::compose::resume_batch_item>::failure(data.error());
  }
  return wh::compose::resume_batch_item{
      .interrupt_context_id = std::move(value.interrupt_context_id),
      .data = std::move(data).value(),
  };
}

} // namespace wh::compose::detail

namespace wh::core {

template <> struct any_owned_traits<wh::compose::resume_patch> {
  [[nodiscard]] static auto into_owned(const wh::compose::resume_patch &value)
      -> wh::core::result<wh::compose::resume_patch> {
    return wh::compose::detail::into_owned_resume_patch(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::resume_patch &&value)
      -> wh::core::result<wh::compose::resume_patch> {
    return wh::compose::detail::into_owned_resume_patch(std::move(value));
  }
};

template <> struct any_owned_traits<wh::compose::resume_batch_item> {
  [[nodiscard]] static auto into_owned(const wh::compose::resume_batch_item &value)
      -> wh::core::result<wh::compose::resume_batch_item> {
    return wh::compose::detail::into_owned_resume_batch_item(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::resume_batch_item &&value)
      -> wh::core::result<wh::compose::resume_batch_item> {
    return wh::compose::detail::into_owned_resume_batch_item(std::move(value));
  }
};

} // namespace wh::core
