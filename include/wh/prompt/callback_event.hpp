// Defines prompt callback payload fields used to report template name,
// variable counts, rendered message counts, and missing-variable errors.
#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include "wh/core/callback.hpp"

namespace wh::prompt {

/// Data contract for `prompt_callback_event`.
struct prompt_callback_event {
  /// Template identifier used for current render call.
  std::string template_name{};
  /// Number of variables available in template context.
  std::size_t variable_count{0U};
  /// Number of rendered messages produced successfully.
  std::size_t rendered_message_count{0U};
  /// Template name that failed during render (when applicable).
  std::string failed_template{};
  /// Variable name that triggered render failure (when applicable).
  std::string failed_variable{};
};

} // namespace wh::prompt
