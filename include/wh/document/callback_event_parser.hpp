// Defines callback payload fields emitted by document parser stages,
// including format hints, batch counts, and parse error context.
#pragma once

#include <cstddef>
#include <string>

namespace wh::document {

/// Data contract for `parser_callback_event`.
struct parser_callback_event {
  /// Source URI associated with parsed content.
  std::string uri{};
  /// Number of input bytes received by parser stage.
  std::size_t input_bytes{0U};
  /// Number of parsed documents emitted by parser stage.
  std::size_t output_count{0U};
};

} // namespace wh::document
