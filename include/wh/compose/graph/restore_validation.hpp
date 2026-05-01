// Defines strict checkpoint restore validation against the current graph shape.
#pragma once

#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/graph/restore_check.hpp"

namespace wh::compose {

using restore_diff_kind = restore_check::restore_diff_kind;
using restore_diff_entry = restore_check::restore_diff_entry;
using restore_diff = restore_check::restore_diff;
using restore_issue_kind = restore_check::issue_kind;
using restore_issue = restore_check::issue;
using restore_validation_result = restore_check::result;

/// Validates whether the current graph can safely restore `checkpoint`.
[[nodiscard]] inline auto validate_restore(const graph &current, const checkpoint_state &checkpoint)
    -> wh::core::result<restore_validation_result> {
  if (!current.compiled()) {
    restore_validation_result result{};
    restore_check::append_issue(result, restore_issue_kind::graph_changed, "graph",
                                "current graph must be compiled before restore validation");
    return result;
  }
  return restore_check::validate(current.restore_shape(), checkpoint);
}

} // namespace wh::compose
