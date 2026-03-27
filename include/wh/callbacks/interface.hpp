// Defines the callback protocol surface used by callback helpers.
#pragma once

#include <type_traits>
#include <utility>

#include "wh/core/callback.hpp"
#include "wh/core/callback.hpp"

namespace wh::callbacks {

/// Lifecycle stage alias reused by callback helpers.
using stage = wh::core::callback_stage;
/// Borrowed callback payload view alias.
using event_view = wh::core::callback_event_view;
/// Owning callback payload alias.
using event_payload = wh::core::callback_event_payload;
/// Per-event callback run context.
using run_info = wh::core::callback_run_info;
/// Lightweight run-info metadata carried by callback sinks/contexts.
using run_metadata = wh::core::callback_run_metadata;
/// Stage filter function alias.
using timing_checker = wh::core::callback_timing_checker;
/// Broadcast stage callback alias.
using stage_view_callback = wh::core::stage_view_callback;
/// Single-consumer stage callback alias.
using stage_payload_callback = wh::core::stage_payload_callback;
/// Per-stage callback table alias.
using stage_callbacks = wh::core::stage_callbacks;
/// Callback registration config alias.
using callback_config = wh::core::callback_config;

template <typename timing_checker_t>
/// Concept alias for callback stage timing filters.
concept TimingChecker = wh::core::TimingChecker<timing_checker_t>;

template <typename callback_t>
/// Concept alias for `(stage, event_view)` callbacks.
concept StageViewCallbackLike =
    wh::core::StageViewCallbackLike<callback_t>;

template <typename callback_t>
/// Concept alias for `(stage, event_payload&&)` callbacks.
concept StagePayloadCallbackLike =
    wh::core::StagePayloadCallbackLike<callback_t>;

/// Returns true for callback stages that execute in reverse registration order.
[[nodiscard]] constexpr auto is_reverse_stage(const stage current_stage) noexcept
    -> bool {
  return wh::core::is_reverse_callback_stage(current_stage);
}

template <typename payload_t>
/// Builds an event view from mutable callback payload.
[[nodiscard]] inline auto make_event_view(payload_t &payload) noexcept
    -> event_view {
  return wh::core::make_callback_event_view(payload);
}

template <typename payload_t>
/// Builds an event view from const callback payload.
[[nodiscard]] inline auto make_event_view(const payload_t &payload) noexcept
    -> event_view {
  return wh::core::make_callback_event_view(payload);
}

template <typename payload_t>
  requires(!std::is_lvalue_reference_v<payload_t>)
/// Prevents creating dangling callback-event views from rvalues.
[[nodiscard]] inline auto make_event_view(payload_t &&) noexcept -> event_view =
    delete;

template <typename payload_t>
/// Builds an owning event payload by type-erasing `payload`.
[[nodiscard]] inline auto make_event_payload(payload_t &&payload)
    -> event_payload {
  return wh::core::make_callback_event_payload(std::forward<payload_t>(payload));
}

template <typename value_t>
/// Returns typed payload pointer when runtime type matches.
[[nodiscard]] inline auto event_get_if(const event_payload &payload)
    -> const value_t * {
  return wh::core::callback_event_get_if<value_t>(payload);
}

template <typename value_t>
/// Returns mutable typed payload pointer when runtime type matches.
[[nodiscard]] inline auto event_get_if(event_payload &payload)
    -> value_t * {
  return wh::core::callback_event_get_if<value_t>(payload);
}

template <typename value_t>
/// Extracts payload by value or returns `errc::type_mismatch`.
[[nodiscard]] inline auto event_as(const event_payload &payload)
    -> wh::core::result<value_t> {
  return wh::core::callback_event_as<value_t>(payload);
}

template <typename value_t>
/// Moves payload by value or returns `errc::type_mismatch`.
[[nodiscard]] inline auto event_as(event_payload &&payload)
    -> wh::core::result<value_t> {
  return wh::core::callback_event_as<value_t>(std::move(payload));
}

template <typename value_t>
/// Extracts payload as immutable reference wrapper.
[[nodiscard]] inline auto event_cref_as(const event_payload &payload)
    -> wh::core::result<std::reference_wrapper<const value_t>> {
  return wh::core::callback_event_cref_as<value_t>(payload);
}

} // namespace wh::callbacks
