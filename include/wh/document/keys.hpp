// Defines shared document metadata keys used by retrieval and indexing
// workflows.
#pragma once

#include <string_view>

namespace wh::document {

/// Metadata key pointing from one child chunk back to its parent document.
inline constexpr std::string_view parent_id_metadata_key = "__parent_id__";

/// Metadata key holding one generated child-document identifier.
inline constexpr std::string_view sub_id_metadata_key = "__sub_id__";

} // namespace wh::document
