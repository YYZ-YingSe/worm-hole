// Defines callback builder for composing stage callbacks into one table.
#pragma once

#include <concepts>
#include <utility>

#include "wh/callbacks/interface.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::callbacks {

namespace detail {

template <typename callback_t>
/// Concept for `(stage, event)` stage callbacks.
concept StageCallbackLike =
    requires(callback_t callback, const stage current_stage,
             const event_view event, const run_info &info) {
      { callback(current_stage, event, info) } -> std::same_as<void>;
    };

} // namespace detail

/// Builder that assembles stage callbacks into immutable callback table.
class stage_callback_builder {
public:
  /// Stage callback signature used by builder stage methods.
  using stage_callback = stage_view_callback;

  stage_callback_builder() = default;

  template <detail::StageCallbackLike callback_t>
  /// Installs `start` stage callback.
  auto on_start(callback_t &&callback) -> stage_callback_builder & {
    callbacks_.on_start = stage_callback{std::forward<callback_t>(callback)};
    return *this;
  }

  template <detail::StageCallbackLike callback_t>
  /// Installs `end` stage callback.
  auto on_end(callback_t &&callback) -> stage_callback_builder & {
    callbacks_.on_end = stage_callback{std::forward<callback_t>(callback)};
    return *this;
  }

  template <detail::StageCallbackLike callback_t>
  /// Installs `error` stage callback.
  auto on_error(callback_t &&callback) -> stage_callback_builder & {
    callbacks_.on_error = stage_callback{std::forward<callback_t>(callback)};
    return *this;
  }

  template <detail::StageCallbackLike callback_t>
  /// Installs `stream_start` stage callback.
  auto on_stream_start(callback_t &&callback) -> stage_callback_builder & {
    callbacks_.on_stream_start =
        stage_callback{std::forward<callback_t>(callback)};
    return *this;
  }

  template <detail::StageCallbackLike callback_t>
  /// Installs `stream_end` stage callback.
  auto on_stream_end(callback_t &&callback) -> stage_callback_builder & {
    callbacks_.on_stream_end =
        stage_callback{std::forward<callback_t>(callback)};
    return *this;
  }

  /// Clears all stage callbacks.
  auto reset() -> stage_callback_builder & {
    callbacks_ = {};
    return *this;
  }

  /// Returns true when no stage callback is installed.
  [[nodiscard]] auto empty() const noexcept -> bool {
    return !static_cast<bool>(callbacks_.on_start) &&
           !static_cast<bool>(callbacks_.on_end) &&
           !static_cast<bool>(callbacks_.on_error) &&
           !static_cast<bool>(callbacks_.on_stream_start) &&
           !static_cast<bool>(callbacks_.on_stream_end);
  }

  /// Builds stage callback table from current builder state.
  [[nodiscard]] auto build_callbacks() -> wh::core::result<stage_callbacks> {
    if (empty()) {
      return wh::core::result<stage_callbacks>::failure(
          wh::core::errc::not_found);
    }
    return std::move(callbacks_);
  }

private:
  /// Installed stage callbacks.
  stage_callbacks callbacks_{};
};

} // namespace wh::callbacks
