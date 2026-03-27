// Defines callback payload fields emitted by document transformer stages,
// including transformation status and per-document processing metadata.
#pragma once

#include <cstddef>

namespace wh::document {

/// Data contract for `transformer_callback_event`.
struct transformer_callback_event {
  /// Number of inputs consumed by transformer stage.
  std::size_t input_count{0U};
  /// Number of outputs produced by transformer stage.
  std::size_t output_count{0U};
};

} // namespace wh::document
