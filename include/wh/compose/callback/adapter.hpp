// Defines compose-local callback adapters for start/end/error hook bundles.
#pragma once

#include "wh/compose/types.hpp"
#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

/// Callback adapter that unifies start/end/error hooks for compose helpers.
struct callback_adapter {
  /// Start hook called before node execution.
  wh::core::callback_function<wh::core::result<void>(const graph_value &, wh::core::run_context &)
                                  const>
      on_start{nullptr};
  /// End hook called after successful execution.
  wh::core::callback_function<wh::core::result<void>(const graph_value &, wh::core::run_context &)
                                  const>
      on_end{nullptr};
  /// Error hook called when node execution fails.
  wh::core::callback_function<wh::core::result<void>(const wh::core::error_code,
                                                     wh::core::run_context &) const>
      on_error{nullptr};

  /// Emits start hook when configured.
  [[nodiscard]] auto emit_start(const graph_value &input, wh::core::run_context &context) const
      -> wh::core::result<void> {
    if (!on_start) {
      return {};
    }
    return on_start(input, context);
  }

  /// Emits end hook when configured.
  [[nodiscard]] auto emit_end(const graph_value &output, wh::core::run_context &context) const
      -> wh::core::result<void> {
    if (!on_end) {
      return {};
    }
    return on_end(output, context);
  }

  /// Emits error hook when configured.
  [[nodiscard]] auto emit_error(const wh::core::error_code error,
                                wh::core::run_context &context) const -> wh::core::result<void> {
    if (!on_error) {
      return {};
    }
    return on_error(error, context);
  }
};

} // namespace wh::compose
