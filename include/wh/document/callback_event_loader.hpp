// Defines callback payload fields emitted by document loader stages,
// including URI/source information and loader progress metadata.
#pragma once

#include <cstddef>
#include <string>

namespace wh::document {

/// Data contract for `loader_callback_event`.
struct loader_callback_event {
  /// Source URI being loaded.
  std::string uri{};
  /// Number of bytes produced by loader stage.
  std::size_t loaded_bytes{0U};
};

} // namespace wh::document
