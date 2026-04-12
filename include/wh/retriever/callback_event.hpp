// Defines retriever callback payload fields for query metadata, hit counts,
// and retrieval failure context.
#pragma once

#include <cstddef>
#include <string>

namespace wh::retriever {

/// Data contract for `retriever_callback_event`.
struct retriever_callback_event {
  /// Requested top-k size for current retrieval call.
  std::size_t top_k{0U};
  /// Score threshold filter applied to candidates.
  double score_threshold{0.0};
  /// Filter expression used for metadata/content filtering.
  std::string filter{};
  /// Extra contextual text (query/result count) for diagnostics.
  std::string extra{};
};

} // namespace wh::retriever
