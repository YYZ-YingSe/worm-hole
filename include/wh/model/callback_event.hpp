// Defines model callback payload fields shared by on_start/on_end/on_error
// to report model id, usage, chunk count, and execution path metadata.
#pragma once

#include <cstddef>
#include <string>

#include "wh/schema/message/types.hpp"

namespace wh::model {

/// Data contract for `chat_model_callback_event`.
struct chat_model_callback_event {
  /// Effective model identifier used for this call.
  std::string model_id{};
  /// Token usage stats reported/estimated for this call.
  wh::schema::token_usage usage{};
  /// Number of emitted chunks/messages in stream path.
  std::size_t emitted_chunks{0U};
  /// True when callback belongs to stream path.
  bool stream_path{false};
};

} // namespace wh::model
