// Defines typed callback helper templates for registration and dispatch.
#pragma once

#include <concepts>
#include <memory>
#include <utility>

#include "wh/callbacks/interface.hpp"
#include "wh/core/run_context.hpp"

namespace wh::callbacks {

template <typename callback_t, typename event_t>
concept TypedStageCallbackWithStage = requires(callback_t callback, const stage current_stage,
                                               const event_t &event, const run_info &info) {
  { callback(current_stage, event, info) } -> std::same_as<void>;
};

template <typename callback_t, typename event_t>
concept TypedStageCallbackWithoutStage =
    requires(callback_t callback, const event_t &event, const run_info &info) {
      { callback(event, info) } -> std::same_as<void>;
    };

template <typename event_t, typename callback_t>
  requires(TypedStageCallbackWithStage<callback_t, event_t> ||
           TypedStageCallbackWithoutStage<callback_t, event_t>)
/// Builds one typed stage callback that only fires on matching payload type.
[[nodiscard]] inline auto make_typed_stage_callback(callback_t &&callback) -> stage_view_callback {
  using stored_callback_t = std::decay_t<callback_t>;
  auto stored_callback = std::make_shared<stored_callback_t>(std::forward<callback_t>(callback));

  return [stored_callback](const stage current_stage, const event_view event,
                           const run_info &info) -> void {
    const auto *typed = event.get_if<event_t>();
    if (typed == nullptr) {
      return;
    }

    if constexpr (TypedStageCallbackWithStage<stored_callback_t, event_t>) {
      (*stored_callback)(current_stage, *typed, info);
    } else {
      (*stored_callback)(*typed, info);
    }
  };
}

template <typename event_t, typename callback_t>
  requires(TypedStageCallbackWithStage<callback_t, event_t> ||
           TypedStageCallbackWithoutStage<callback_t, event_t>)
/// Builds typed stage-callback table that only fires on matching payload type.
[[nodiscard]] inline auto make_typed_stage_callbacks(callback_t &&callback) -> stage_callbacks {
  auto callback_fn = make_typed_stage_callback<event_t>(std::forward<callback_t>(callback));

  stage_callbacks callbacks{};
  callbacks.on_start = callback_fn;
  callbacks.on_end = callback_fn;
  callbacks.on_error = callback_fn;
  callbacks.on_stream_start = callback_fn;
  callbacks.on_stream_end = std::move(callback_fn);
  return callbacks;
}

template <typename event_t, typename config_t, typename callback_t>
  requires(TypedStageCallbackWithStage<callback_t, event_t> ||
           TypedStageCallbackWithoutStage<callback_t, event_t>) &&
          wh::core::CallbackConfigLike<wh::core::remove_cvref_t<config_t>>
/// Registers one typed local stage-callback table on context callback manager.
[[nodiscard]] inline auto register_typed_local_callbacks(wh::core::run_context &&context,
                                                         config_t &&config, callback_t &&callback)
    -> wh::core::result<wh::core::run_context> {
  return wh::core::register_local_callbacks(
      std::move(context), std::forward<config_t>(config),
      make_typed_stage_callbacks<event_t>(std::forward<callback_t>(callback)));
}

template <typename event_t, typename config_t, typename callback_t>
  requires(TypedStageCallbackWithStage<callback_t, event_t> ||
           TypedStageCallbackWithoutStage<callback_t, event_t>) &&
          wh::core::CallbackConfigLike<wh::core::remove_cvref_t<config_t>>
/// Registers one typed local stage-callback table using copied run context.
[[nodiscard]] inline auto register_typed_local_callbacks(const wh::core::run_context &context,
                                                         config_t &&config, callback_t &&callback)
    -> wh::core::result<wh::core::run_context> {
  return wh::core::register_local_callbacks(
      context, std::forward<config_t>(config),
      make_typed_stage_callbacks<event_t>(std::forward<callback_t>(callback)));
}

} // namespace wh::callbacks
