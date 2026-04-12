// Defines embedding callback payload fields for batch size, usage, and
// failure metadata emitted during embedding generation.
#pragma once

#include <string>

#include "wh/schema/message.hpp"

namespace wh::embedding {

/// Data contract for `embedding_callback_event`.
struct embedding_callback_event {
  /// Effective model identifier used for embedding call.
  std::string model_id{};
  /// Token usage stats reported/estimated for the call.
  wh::schema::token_usage usage{};
  /// Number of input items in current embedding batch.
  std::size_t batch_size{0U};
};

} // namespace wh::embedding
