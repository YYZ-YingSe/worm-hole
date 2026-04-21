// Aggregates callback protocol, manager, sink helpers, injection, and typed
// helpers.
#pragma once

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "wh/callbacks/aspect_inject.hpp"
#include "wh/callbacks/callback_builder.hpp"
#include "wh/callbacks/interface.hpp"
#include "wh/callbacks/manager.hpp"
#include "wh/callbacks/template_helper.hpp"
#include "wh/core/run_context.hpp"

namespace wh::callbacks {

/// Optional callback-manager sink reusable across component execution helpers.
struct callback_sink {
  /// Borrowed manager used by sync call paths that do not outlive the source
  /// context.
  const manager *borrowed{nullptr};
  /// Owned manager snapshot used by async/detached call paths.
  std::optional<manager> owned{};
  /// Optional metadata applied to emitted callback run-info.
  std::optional<run_metadata> metadata{};

  [[nodiscard]] auto has_value() const noexcept -> bool { return manager_ptr() != nullptr; }

  [[nodiscard]] auto manager_ptr() const noexcept -> const manager * {
    if (owned.has_value()) {
      return std::addressof(owned.value());
    }
    return borrowed;
  }
};

/// Returns an empty sink when callback emission is disabled.
[[nodiscard]] inline auto make_callback_sink() -> callback_sink { return {}; }

/// Borrows callback-manager state from the provided run context for sync paths.
[[nodiscard]] inline auto borrow_callback_sink(const wh::core::run_context &context)
    -> callback_sink {
  callback_sink sink{};
  if (context.callbacks.has_value()) {
    sink.borrowed = std::addressof(context.callbacks->manager);
  }
  if (context.callbacks.has_value() && !context.callbacks->metadata.empty()) {
    sink.metadata = context.callbacks->metadata;
  }
  return sink;
}

/// Copies callback-manager state out of the provided run context.
[[nodiscard]] inline auto make_callback_sink(const wh::core::run_context &context)
    -> callback_sink {
  callback_sink sink{};
  if (context.callbacks.has_value()) {
    sink.owned = context.callbacks->manager;
  }
  if (context.callbacks.has_value() && !context.callbacks->metadata.empty()) {
    sink.metadata = context.callbacks->metadata;
  }
  return sink;
}

/// Moves callback-manager state out of the provided run context.
[[nodiscard]] inline auto make_callback_sink(wh::core::run_context &&context) -> callback_sink {
  callback_sink sink{};
  if (context.callbacks.has_value()) {
    sink.owned = std::move(context.callbacks->manager);
  }
  if (context.callbacks.has_value() && !context.callbacks->metadata.empty()) {
    sink.metadata = std::move(context.callbacks->metadata);
  }
  return sink;
}

template <typename payload_t>
/// Emits one callback event through a detached callback sink.
inline auto emit(const callback_sink &sink, const stage current_stage, const payload_t &payload,
                 const run_info &info) -> void {
  const auto *sink_manager = sink.manager_ptr();
  if (sink_manager == nullptr) {
    return;
  }
  if (sink.metadata.has_value()) {
    sink_manager->dispatch(current_stage, make_event_view(payload),
                           apply_callback_run_metadata(info, *sink.metadata));
    return;
  }
  sink_manager->dispatch(current_stage, make_event_view(payload), info);
}

template <typename options_t>
concept component_options_provider = requires(const options_t &options) {
  { options.component_options() } -> std::same_as<const wh::core::component_options &>;
};

template <component_options_provider options_t>
[[nodiscard]] inline auto apply_component_run_info(run_info info, const options_t &options)
    -> run_info {
  return wh::core::apply_component_run_info(std::move(info), options.component_options());
}

template <component_options_provider options_t>
[[nodiscard]] inline auto callbacks_enabled(const options_t &options) -> bool {
  return options.component_options().resolve_view().callbacks_enabled;
}

template <component_options_provider options_t>
[[nodiscard]] inline auto filter_callback_sink(callback_sink sink, const options_t &options)
    -> callback_sink {
  if (callbacks_enabled(options)) {
    return sink;
  }
  return make_callback_sink();
}

template <typename state_t>
  requires requires(const state_t &state) {
    state.event;
    state.run_info;
  }
/// Emits one callback event from a `{event, run_info}` state bundle.
inline auto emit(const callback_sink &sink, const stage current_stage, const state_t &state)
    -> void {
  emit(sink, current_stage, state.event, state.run_info);
}

} // namespace wh::callbacks
