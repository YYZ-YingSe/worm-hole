#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "wh/core/address.hpp"
#include "wh/core/any.hpp"
#include "wh/core/component/types.hpp"
#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::core {

/// Lifecycle stages where callbacks can be triggered.
enum class callback_stage : std::uint8_t {
  /// Before operation execution starts.
  start = 0U,
  /// After operation completes successfully.
  end,
  /// When operation ends with error.
  error,
  /// Before first stream chunk is emitted.
  stream_start,
  /// After stream completion signal is emitted.
  stream_end,
};

/// Returns true for stages that should execute in reverse registration order.
[[nodiscard]] constexpr auto
is_reverse_callback_stage(const callback_stage stage) noexcept -> bool {
  return stage == callback_stage::start;
}

/// Non-owning typed view used by callback dispatch.
using callback_event_view = any;

/// Helper to build `callback_event_view` from mutable lvalue payload.
template <typename payload_t>
[[nodiscard]] inline auto make_callback_event_view(payload_t &value) noexcept
    -> callback_event_view {
  return forward_as_any(value);
}

/// Helper to build `callback_event_view` from const lvalue payload.
template <typename payload_t>
[[nodiscard]] inline auto
make_callback_event_view(const payload_t &value) noexcept
    -> callback_event_view {
  return forward_as_any(value);
}

/// Prevent creating dangling callback-event views from rvalues.
template <typename payload_t>
  requires(!std::is_lvalue_reference_v<payload_t>)
[[nodiscard]] inline auto make_callback_event_view(payload_t &&) noexcept
    -> callback_event_view = delete;

/// Owning callback payload used by callback bridges.
using callback_event_payload = any;

/// Creates callback payload by type-erasing `value`.
template <typename value_t>
[[nodiscard]] inline auto make_callback_event_payload(value_t &&value)
    -> callback_event_payload {
  return any{std::forward<value_t>(value)};
}

/// Returns typed payload pointer when stored type matches `value_t`.
template <typename value_t>
[[nodiscard]] inline auto
callback_event_get_if(const callback_event_payload &extra) -> const value_t * {
  return any_cast<value_t>(&extra);
}

/// Returns mutable typed payload pointer when stored type matches `value_t`.
template <typename value_t>
[[nodiscard]] inline auto callback_event_get_if(callback_event_payload &extra)
    -> value_t * {
  return any_cast<value_t>(&extra);
}

/// Extracts callback payload by value or returns `errc::type_mismatch`.
template <typename value_t>
[[nodiscard]] inline auto callback_event_as(const callback_event_payload &extra)
    -> result<value_t> {
  if (const auto *typed = any_cast<value_t>(&extra); typed != nullptr) {
    return *typed;
  }
  return result<value_t>::failure(errc::type_mismatch);
}

/// Moves callback payload by value or returns `errc::type_mismatch`.
template <typename value_t>
[[nodiscard]] inline auto callback_event_as(callback_event_payload &&extra)
    -> result<value_t> {
  if (auto *typed = any_cast<value_t>(&extra); typed != nullptr) {
    return std::move(*typed);
  }
  return result<value_t>::failure(errc::type_mismatch);
}

/// Extracts callback payload as immutable reference wrapper.
template <typename value_t>
[[nodiscard]] inline auto
callback_event_cref_as(const callback_event_payload &extra)
    -> result<std::reference_wrapper<const value_t>> {
  if (const auto *typed = any_cast<value_t>(&extra); typed != nullptr) {
    return std::cref(*typed);
  }
  return result<std::reference_wrapper<const value_t>>::failure(
      errc::type_mismatch);
}

/// Predicate that decides whether a callback should run for a stage.
using callback_timing_checker =
    wh::core::callback_function<bool(callback_stage) const>;

/// Runtime callback context injected by components for each emitted event.
struct callback_run_info {
  /// Node/operation display name.
  std::string name{};
  /// Component implementation type name.
  std::string type{};
  /// Component category.
  wh::core::component_kind component{wh::core::component_kind::custom};
  /// Distributed trace id associated with the current call.
  std::string trace_id{};
  /// Distributed span id associated with the current call.
  std::string span_id{};
  /// Parent span id when this call is nested under another span.
  std::string parent_span_id{};
  /// Hierarchical node path associated with the current call.
  wh::core::address node_path{};
  /// User-defined custom fields for observability extensions.
  std::unordered_map<std::string, wh::core::any,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      custom{};
};

/// Lightweight callback metadata injected on top of emitted run info.
struct callback_run_metadata {
  /// Optional trace id override.
  std::string trace_id{};
  /// Optional span id override.
  std::string span_id{};
  /// Optional parent span id override.
  std::string parent_span_id{};
  /// Optional node path override.
  wh::core::address node_path{};

  [[nodiscard]] auto empty() const noexcept -> bool {
    return trace_id.empty() && span_id.empty() && parent_span_id.empty() &&
           node_path.empty();
  }
};

/// Applies callback metadata on top of one emitted run-info payload.
[[nodiscard]] inline auto
apply_callback_run_metadata(callback_run_info run_info,
                            const callback_run_metadata &metadata)
    -> callback_run_info {
  if (!metadata.trace_id.empty()) {
    run_info.trace_id = metadata.trace_id;
  }
  if (!metadata.span_id.empty()) {
    run_info.span_id = metadata.span_id;
  }
  if (!metadata.parent_span_id.empty()) {
    run_info.parent_span_id = metadata.parent_span_id;
  }
  if (!metadata.node_path.empty()) {
    run_info.node_path = metadata.node_path;
  }
  return run_info;
}

/// Applies resolved component metadata on top of one callback run-info payload.
[[nodiscard]] inline auto
apply_component_run_info(callback_run_info run_info,
                         const resolved_component_options_view options)
    -> callback_run_info {
  if (!options.trace_id.empty()) {
    run_info.trace_id = std::string{options.trace_id};
  }
  if (!options.span_id.empty()) {
    run_info.span_id = std::string{options.span_id};
  }
  return run_info;
}

/// Applies resolved component metadata on top of one callback run-info payload.
[[nodiscard]] inline auto
apply_component_run_info(callback_run_info run_info,
                         const component_options &options)
    -> callback_run_info {
  return apply_component_run_info(std::move(run_info), options.resolve_view());
}

/// Callback signature used by dispatch pipeline.
using stage_view_callback = wh::core::callback_function<void(
    callback_stage, callback_event_view, const callback_run_info &) const>;
/// Single-consumer callback signature for owning payload dispatch.
using stage_payload_callback =
    wh::core::callback_function<void(callback_stage, callback_event_payload &&,
                                     const callback_run_info &) const>;

/// Per-stage callback table used by registration-time stage expansion.
struct stage_callbacks {
  /// Callback for `callback_stage::start`.
  stage_view_callback on_start{nullptr};
  /// Callback for `callback_stage::end`.
  stage_view_callback on_end{nullptr};
  /// Callback for `callback_stage::error`.
  stage_view_callback on_error{nullptr};
  /// Callback for `callback_stage::stream_start`.
  stage_view_callback on_stream_start{nullptr};
  /// Callback for `callback_stage::stream_end`.
  stage_view_callback on_stream_end{nullptr};
};

/// Callback registration options.
struct callback_config {
  /// Stage filter predicate controlling callback execution.
  callback_timing_checker timing_checker{nullptr};
  /// Optional callback name for diagnostics.
  std::string name{};
};

/// Fatal error payload emitted by callback dispatch layer.
struct callback_fatal_error {
  /// Framework error code mapped from thrown exception.
  error_code code{errc::internal_error};
  /// Exception message text when available.
  std::string exception_message{};
  /// Best-effort stack trace captured at failure site.
  std::string call_stack{};

  /// Formats fatal error for logging/debug output.
  [[nodiscard]] auto to_string() const -> std::string {
    return std::string{"code="} + code.message() +
           ", exception=" + exception_message + ", stack=" + call_stack;
  }
};

} // namespace wh::core
