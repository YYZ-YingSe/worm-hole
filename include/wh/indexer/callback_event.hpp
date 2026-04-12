// Defines indexer callback payload fields for document counts, upsert status,
// and indexing failure context.
#pragma once

#include <cstddef>

namespace wh::indexer {

/// Data contract for `indexer_callback_event`.
struct indexer_callback_event {
  /// Number of documents requested for indexing.
  std::size_t batch_size{0U};
  /// Number of documents successfully written.
  std::size_t success_count{0U};
  /// Number of documents that failed to write.
  std::size_t failure_count{0U};
};

} // namespace wh::indexer
