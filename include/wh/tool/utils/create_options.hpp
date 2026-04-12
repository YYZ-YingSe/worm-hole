// Provides declarations and utilities for `wh/tool/utils/create_options.hpp`.
#pragma once

#include "wh/tool/options.hpp"

namespace wh::tool::utils {

/// Merges base options with call-time override options.
[[nodiscard]] inline auto create_options(const tool_options &base,
                                         const tool_options &call_override)
    -> tool_options {
  tool_options merged = base;
  merged.set_call_override(call_override.resolve());
  return merged;
}

} // namespace wh::tool::utils
