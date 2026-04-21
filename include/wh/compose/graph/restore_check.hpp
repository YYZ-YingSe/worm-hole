// Defines core graph-restore validation helpers shared by public and runtime
// paths.
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/compose/graph/keys.hpp"
#include "wh/compose/graph/restore_shape.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/core/address.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose::restore_check {

/// Typed diff category emitted by restore-shape comparison.
enum class restore_diff_kind : std::uint8_t {
  /// Graph-level restore option changed.
  graph_option = 0U,
  /// Node exists only in current graph.
  node_added,
  /// Node exists only in checkpoint payload.
  node_removed,
  /// Node restore-relevant shape changed.
  node_shape,
  /// Edge exists only in current graph.
  edge_added,
  /// Edge exists only in checkpoint payload.
  edge_removed,
  /// Edge restore-relevant shape changed.
  edge_shape,
  /// Branch destination set changed.
  branch_destinations,
};

/// One typed restore diff entry.
struct restore_diff_entry {
  /// Diff category.
  restore_diff_kind kind{restore_diff_kind::graph_option};
  /// Subject path/key/edge label affected by this change.
  std::string subject{};
  /// Field/slot name within `subject`.
  std::string detail{};
  /// Previous restore-visible value.
  std::string before{};
  /// Current restore-visible value.
  std::string after{};
};

/// Ordered typed diff produced by one restore-shape comparison.
struct restore_diff {
  /// Ordered diff entries.
  std::vector<restore_diff_entry> entries{};

  /// Returns entry count for one diff kind.
  [[nodiscard]] auto count(const restore_diff_kind kind) const noexcept -> std::size_t {
    return static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(),
        [kind](const restore_diff_entry &entry) -> bool { return entry.kind == kind; }));
  }

  /// Returns true when at least one entry of `kind` exists.
  [[nodiscard]] auto contains(const restore_diff_kind kind) const noexcept -> bool {
    return std::any_of(
        entries.begin(), entries.end(),
        [kind](const restore_diff_entry &entry) -> bool { return entry.kind == kind; });
  }
};

/// Restore validation issue category.
enum class issue_kind : std::uint8_t {
  /// Current graph restore shape differs from the checkpoint payload.
  graph_changed = 0U,
  /// One persisted node-state target no longer exists in the current graph.
  missing_node_state,
  /// One persisted runtime node-input target no longer exists in the current
  /// graph.
  missing_node_input,
  /// One persisted resume target address no longer exists in the current graph.
  missing_resume_target,
  /// One persisted interrupt target address no longer exists in the current
  /// graph.
  missing_interrupt_target,
};

/// One restore validation issue.
struct issue {
  /// Issue category.
  issue_kind kind{issue_kind::graph_changed};
  /// Subject path/address tied to this issue.
  std::string subject{};
  /// Human-readable diagnostic message.
  std::string message{};
};

/// Structured restore validation result.
struct result {
  /// True when the current graph can safely restore the checkpoint.
  bool restorable{true};
  /// Restore-visible diff between checkpoint shape and current graph shape.
  restore_diff diff{};
  /// Ordered validation issues.
  std::vector<issue> issues{};
};

using path_set = std::unordered_set<std::string, wh::core::transparent_string_hash,
                                    wh::core::transparent_string_equal>;

[[nodiscard]] inline auto bool_text(const bool value) -> std::string {
  return value ? "true" : "false";
}

[[nodiscard]] inline auto mode_text(const graph_runtime_mode mode) -> std::string_view {
  return mode == graph_runtime_mode::pregel ? "pregel" : "dag";
}

[[nodiscard]] inline auto dispatch_policy_text(const graph_dispatch_policy policy)
    -> std::string_view {
  return policy == graph_dispatch_policy::next_wave ? "next_wave" : "same_wave";
}

[[nodiscard]] inline auto trigger_mode_text(const graph_trigger_mode mode) -> std::string_view {
  return mode == graph_trigger_mode::all_predecessors ? "all_predecessors" : "any_predecessor";
}

[[nodiscard]] inline auto fan_in_policy_text(const graph_fan_in_policy policy) -> std::string_view {
  switch (policy) {
  case graph_fan_in_policy::allow_partial:
    return "allow_partial";
  case graph_fan_in_policy::require_all_sources:
    return "require_all_sources";
  }
  return "allow_partial";
}

[[nodiscard]] inline auto node_kind_text(const node_kind kind) -> std::string_view {
  switch (kind) {
  case node_kind::component:
    return "component";
  case node_kind::lambda:
    return "lambda";
  case node_kind::subgraph:
    return "subgraph";
  case node_kind::tools:
    return "tools";
  case node_kind::passthrough:
    return "passthrough";
  }
  return "component";
}

[[nodiscard]] inline auto join_shape_path(const std::string_view prefix, const std::string_view key)
    -> std::string {
  if (prefix.empty()) {
    return std::string{key};
  }
  std::string path{prefix};
  path.push_back('/');
  path += key;
  return path;
}

[[nodiscard]] inline auto edge_subject(const std::string_view prefix,
                                       const graph_restore_edge &edge) -> std::string {
  return join_shape_path(prefix, edge.from) + "->" + join_shape_path(prefix, edge.to);
}

inline auto collect_shape_paths(const graph_restore_shape &shape, const std::string_view prefix,
                                path_set &paths) -> void {
  paths.insert(join_shape_path(prefix, graph_start_node_key));
  paths.insert(join_shape_path(prefix, graph_end_node_key));
  for (const auto &node : shape.nodes) {
    const auto path = join_shape_path(prefix, node.key);
    paths.insert(path);
    const auto nested = shape.subgraphs.find(node.key);
    if (nested != shape.subgraphs.end()) {
      collect_shape_paths(nested->second, path, paths);
    }
  }
}

[[nodiscard]] inline auto strip_graph_root(const wh::core::address &address)
    -> std::optional<std::string> {
  const auto segments = address.segments();
  if (segments.empty()) {
    return std::nullopt;
  }
  if (segments.size() == 1U) {
    return std::string{};
  }
  std::string path{};
  for (std::size_t index = 1U; index < segments.size(); ++index) {
    if (index > 1U) {
      path.push_back('/');
    }
    path += segments[index];
  }
  return path;
}

inline auto append_issue(result &validation, const issue_kind kind, std::string subject,
                         std::string message) -> void {
  validation.restorable = false;
  validation.issues.push_back(issue{
      .kind = kind,
      .subject = std::move(subject),
      .message = std::move(message),
  });
}

inline auto append_diff(restore_diff &restore_diff, const restore_diff_kind kind,
                        std::string subject, std::string detail, std::string before,
                        std::string after) -> void {
  restore_diff.entries.push_back(restore_diff_entry{
      .kind = kind,
      .subject = std::move(subject),
      .detail = std::move(detail),
      .before = std::move(before),
      .after = std::move(after),
  });
}

inline auto compare_restore_shapes(restore_diff &restore_diff, const graph_restore_shape &baseline,
                                   const graph_restore_shape &current,
                                   const std::string_view prefix) -> void;

inline auto compare_graph_options(restore_diff &restore_diff, const graph_restore_shape &baseline,
                                  const graph_restore_shape &current, const std::string_view prefix)
    -> void {
  const std::string subject = prefix.empty() ? std::string{"graph"} : std::string{prefix};
  if (baseline.options.boundary.input != current.options.boundary.input ||
      baseline.options.boundary.output != current.options.boundary.output) {
    append_diff(restore_diff, restore_diff_kind::graph_option, subject, "boundary",
                std::string{to_string(baseline.options.boundary.input)} + "->" +
                    std::string{to_string(baseline.options.boundary.output)},
                std::string{to_string(current.options.boundary.input)} + "->" +
                    std::string{to_string(current.options.boundary.output)});
  }
  if (baseline.options.mode != current.options.mode) {
    append_diff(restore_diff, restore_diff_kind::graph_option, subject, "mode",
                std::string{mode_text(baseline.options.mode)},
                std::string{mode_text(current.options.mode)});
  }
  if (baseline.options.dispatch_policy != current.options.dispatch_policy) {
    append_diff(restore_diff, restore_diff_kind::graph_option, subject, "dispatch_policy",
                std::string{dispatch_policy_text(baseline.options.dispatch_policy)},
                std::string{dispatch_policy_text(current.options.dispatch_policy)});
  }
  if (baseline.options.trigger_mode != current.options.trigger_mode) {
    append_diff(restore_diff, restore_diff_kind::graph_option, subject, "trigger_mode",
                std::string{trigger_mode_text(baseline.options.trigger_mode)},
                std::string{trigger_mode_text(current.options.trigger_mode)});
  }
  if (baseline.options.fan_in_policy != current.options.fan_in_policy) {
    append_diff(restore_diff, restore_diff_kind::graph_option, subject, "fan_in_policy",
                std::string{fan_in_policy_text(baseline.options.fan_in_policy)},
                std::string{fan_in_policy_text(current.options.fan_in_policy)});
  }
}

inline auto compare_nodes(restore_diff &restore_diff, const graph_restore_shape &baseline,
                          const graph_restore_shape &current, const std::string_view prefix)
    -> void {
  std::size_t baseline_index = 0U;
  std::size_t current_index = 0U;
  while (baseline_index < baseline.nodes.size() || current_index < current.nodes.size()) {
    if (baseline_index == baseline.nodes.size()) {
      const auto &node = current.nodes[current_index++];
      append_diff(restore_diff, restore_diff_kind::node_added, join_shape_path(prefix, node.key),
                  "node", "", std::string{node_kind_text(node.kind)});
      continue;
    }
    if (current_index == current.nodes.size()) {
      const auto &node = baseline.nodes[baseline_index++];
      append_diff(restore_diff, restore_diff_kind::node_removed, join_shape_path(prefix, node.key),
                  "node", std::string{node_kind_text(node.kind)}, "");
      continue;
    }

    const auto &baseline_node = baseline.nodes[baseline_index];
    const auto &current_node = current.nodes[current_index];
    if (baseline_node.key < current_node.key) {
      append_diff(restore_diff, restore_diff_kind::node_removed,
                  join_shape_path(prefix, baseline_node.key), "node",
                  std::string{node_kind_text(baseline_node.kind)}, "");
      ++baseline_index;
      continue;
    }
    if (current_node.key < baseline_node.key) {
      append_diff(restore_diff, restore_diff_kind::node_added,
                  join_shape_path(prefix, current_node.key), "node", "",
                  std::string{node_kind_text(current_node.kind)});
      ++current_index;
      continue;
    }

    const auto subject = join_shape_path(prefix, baseline_node.key);
    if (baseline_node.kind != current_node.kind) {
      append_diff(restore_diff, restore_diff_kind::node_shape, subject, "kind",
                  std::string{node_kind_text(baseline_node.kind)},
                  std::string{node_kind_text(current_node.kind)});
    }
    if (baseline_node.input_contract != current_node.input_contract) {
      append_diff(restore_diff, restore_diff_kind::node_shape, subject, "input_contract",
                  std::string{to_string(baseline_node.input_contract)},
                  std::string{to_string(current_node.input_contract)});
    }
    if (baseline_node.allow_no_control != current_node.allow_no_control) {
      append_diff(restore_diff, restore_diff_kind::node_shape, subject, "allow_no_control",
                  bool_text(baseline_node.allow_no_control),
                  bool_text(current_node.allow_no_control));
    }
    if (baseline_node.allow_no_data != current_node.allow_no_data) {
      append_diff(restore_diff, restore_diff_kind::node_shape, subject, "allow_no_data",
                  bool_text(baseline_node.allow_no_data), bool_text(current_node.allow_no_data));
    }
    ++baseline_index;
    ++current_index;
  }
}

inline auto compare_edges(restore_diff &restore_diff, const graph_restore_shape &baseline,
                          const graph_restore_shape &current, const std::string_view prefix)
    -> void {
  std::size_t baseline_index = 0U;
  std::size_t current_index = 0U;
  while (baseline_index < baseline.edges.size() || current_index < current.edges.size()) {
    if (baseline_index == baseline.edges.size()) {
      const auto &edge = current.edges[current_index++];
      append_diff(restore_diff, restore_diff_kind::edge_added, edge_subject(prefix, edge), "edge",
                  "", "present");
      continue;
    }
    if (current_index == current.edges.size()) {
      const auto &edge = baseline.edges[baseline_index++];
      append_diff(restore_diff, restore_diff_kind::edge_removed, edge_subject(prefix, edge), "edge",
                  "present", "");
      continue;
    }

    const auto &baseline_edge = baseline.edges[baseline_index];
    const auto &current_edge = current.edges[current_index];
    const auto baseline_key = std::tie(baseline_edge.from, baseline_edge.to);
    const auto current_key = std::tie(current_edge.from, current_edge.to);
    if (baseline_key < current_key) {
      append_diff(restore_diff, restore_diff_kind::edge_removed,
                  edge_subject(prefix, baseline_edge), "edge", "present", "");
      ++baseline_index;
      continue;
    }
    if (current_key < baseline_key) {
      append_diff(restore_diff, restore_diff_kind::edge_added, edge_subject(prefix, current_edge),
                  "edge", "", "present");
      ++current_index;
      continue;
    }

    const auto subject = edge_subject(prefix, baseline_edge);
    if (baseline_edge.no_control != current_edge.no_control) {
      append_diff(restore_diff, restore_diff_kind::edge_shape, subject, "no_control",
                  bool_text(baseline_edge.no_control), bool_text(current_edge.no_control));
    }
    if (baseline_edge.no_data != current_edge.no_data) {
      append_diff(restore_diff, restore_diff_kind::edge_shape, subject, "no_data",
                  bool_text(baseline_edge.no_data), bool_text(current_edge.no_data));
    }
    if (baseline_edge.lowering_kind != current_edge.lowering_kind) {
      append_diff(restore_diff, restore_diff_kind::edge_shape, subject, "lowering_kind",
                  std::string{to_string(baseline_edge.lowering_kind)},
                  std::string{to_string(current_edge.lowering_kind)});
    }
    if (baseline_edge.has_custom_lowering != current_edge.has_custom_lowering) {
      append_diff(restore_diff, restore_diff_kind::edge_shape, subject, "has_custom_lowering",
                  bool_text(baseline_edge.has_custom_lowering),
                  bool_text(current_edge.has_custom_lowering));
    }
    ++baseline_index;
    ++current_index;
  }
}

inline auto compare_branches(restore_diff &restore_diff, const graph_restore_shape &baseline,
                             const graph_restore_shape &current, const std::string_view prefix)
    -> void {
  std::size_t baseline_index = 0U;
  std::size_t current_index = 0U;
  while (baseline_index < baseline.branches.size() || current_index < current.branches.size()) {
    if (baseline_index == baseline.branches.size()) {
      const auto &branch = current.branches[current_index++];
      append_diff(restore_diff, restore_diff_kind::branch_destinations,
                  join_shape_path(prefix, branch.from), "destinations", "",
                  std::to_string(branch.end_nodes.size()) + " destinations");
      continue;
    }
    if (current_index == current.branches.size()) {
      const auto &branch = baseline.branches[baseline_index++];
      append_diff(restore_diff, restore_diff_kind::branch_destinations,
                  join_shape_path(prefix, branch.from), "destinations",
                  std::to_string(branch.end_nodes.size()) + " destinations", "");
      continue;
    }

    const auto &baseline_branch = baseline.branches[baseline_index];
    const auto &current_branch = current.branches[current_index];
    if (baseline_branch.from < current_branch.from) {
      append_diff(restore_diff, restore_diff_kind::branch_destinations,
                  join_shape_path(prefix, baseline_branch.from), "destinations",
                  std::to_string(baseline_branch.end_nodes.size()) + " destinations", "");
      ++baseline_index;
      continue;
    }
    if (current_branch.from < baseline_branch.from) {
      append_diff(restore_diff, restore_diff_kind::branch_destinations,
                  join_shape_path(prefix, current_branch.from), "destinations", "",
                  std::to_string(current_branch.end_nodes.size()) + " destinations");
      ++current_index;
      continue;
    }

    if (baseline_branch.end_nodes != current_branch.end_nodes) {
      auto join_end_nodes = [](const std::vector<std::string> &end_nodes) -> std::string {
        std::string text{};
        for (std::size_t index = 0U; index < end_nodes.size(); ++index) {
          if (index > 0U) {
            text += ",";
          }
          text += end_nodes[index];
        }
        return text;
      };
      append_diff(restore_diff, restore_diff_kind::branch_destinations,
                  join_shape_path(prefix, baseline_branch.from), "destinations",
                  join_end_nodes(baseline_branch.end_nodes),
                  join_end_nodes(current_branch.end_nodes));
    }
    ++baseline_index;
    ++current_index;
  }
}

inline auto compare_subgraphs(restore_diff &restore_diff, const graph_restore_shape &baseline,
                              const graph_restore_shape &current, const std::string_view prefix)
    -> void {
  std::vector<std::string_view> baseline_keys{};
  baseline_keys.reserve(baseline.subgraphs.size());
  for (const auto &[key, _] : baseline.subgraphs) {
    baseline_keys.push_back(key);
  }
  std::sort(baseline_keys.begin(), baseline_keys.end());

  std::vector<std::string_view> current_keys{};
  current_keys.reserve(current.subgraphs.size());
  for (const auto &[key, _] : current.subgraphs) {
    current_keys.push_back(key);
  }
  std::sort(current_keys.begin(), current_keys.end());

  std::size_t baseline_index = 0U;
  std::size_t current_index = 0U;
  while (baseline_index < baseline_keys.size() || current_index < current_keys.size()) {
    if (baseline_index == baseline_keys.size()) {
      const auto key = current_keys[current_index++];
      append_diff(restore_diff, restore_diff_kind::node_shape, join_shape_path(prefix, key),
                  "subgraph", "", "present");
      continue;
    }
    if (current_index == current_keys.size()) {
      const auto key = baseline_keys[baseline_index++];
      append_diff(restore_diff, restore_diff_kind::node_shape, join_shape_path(prefix, key),
                  "subgraph", "present", "");
      continue;
    }

    const auto baseline_key = baseline_keys[baseline_index];
    const auto current_key = current_keys[current_index];
    if (baseline_key < current_key) {
      append_diff(restore_diff, restore_diff_kind::node_shape,
                  join_shape_path(prefix, baseline_key), "subgraph", "present", "");
      ++baseline_index;
      continue;
    }
    if (current_key < baseline_key) {
      append_diff(restore_diff, restore_diff_kind::node_shape, join_shape_path(prefix, current_key),
                  "subgraph", "", "present");
      ++current_index;
      continue;
    }

    const auto baseline_iter = baseline.subgraphs.find(std::string{baseline_key});
    const auto current_iter = current.subgraphs.find(std::string{current_key});
    if (baseline_iter != baseline.subgraphs.end() && current_iter != current.subgraphs.end()) {
      compare_restore_shapes(restore_diff, baseline_iter->second, current_iter->second,
                             join_shape_path(prefix, baseline_key));
    }
    ++baseline_index;
    ++current_index;
  }
}

inline auto compare_restore_shapes(restore_diff &restore_diff, const graph_restore_shape &baseline,
                                   const graph_restore_shape &current,
                                   const std::string_view prefix) -> void {
  compare_graph_options(restore_diff, baseline, current, prefix);
  compare_nodes(restore_diff, baseline, current, prefix);
  compare_edges(restore_diff, baseline, current, prefix);
  compare_branches(restore_diff, baseline, current, prefix);
  compare_subgraphs(restore_diff, baseline, current, prefix);
}

[[nodiscard]] inline auto compare_restore_shapes(const graph_restore_shape &baseline,
                                                 const graph_restore_shape &current)
    -> restore_diff {
  restore_diff restore_diff{};
  compare_restore_shapes(restore_diff, baseline, current, "");
  return restore_diff;
}

[[nodiscard]] inline auto has_blocking_diff(const restore_diff &restore_diff) noexcept -> bool {
  return !restore_diff.entries.empty();
}

[[nodiscard]] inline auto validate(const graph_restore_shape &current_shape,
                                   const checkpoint_state &checkpoint) -> result {
  result validation{};
  validation.diff = compare_restore_shapes(checkpoint.restore_shape, current_shape);
  if (has_blocking_diff(validation.diff)) {
    append_issue(validation, issue_kind::graph_changed, "graph",
                 "current graph restore shape differs from checkpoint payload");
  }

  path_set valid_paths{};
  collect_shape_paths(current_shape, "", valid_paths);

  for (const auto &state : checkpoint.runtime.lifecycle) {
    if (!state.key.empty() && !valid_paths.contains(state.key)) {
      append_issue(validation, issue_kind::missing_node_state, state.key,
                   "checkpoint node state targets a node that no longer exists");
    }
  }

  const auto validate_pending_inputs = [&](const checkpoint_pending_inputs &pending) -> void {
    for (const auto &node_input : pending.nodes) {
      const auto &node_key = node_input.key;
      if (!valid_paths.contains(node_key)) {
        append_issue(validation, issue_kind::missing_node_input, node_key,
                     "checkpoint node input targets a node that no longer exists");
      }
    }
  };
  if (checkpoint.runtime.dag.has_value()) {
    validate_pending_inputs(checkpoint.runtime.dag->pending_inputs);
  }
  if (checkpoint.runtime.pregel.has_value()) {
    validate_pending_inputs(checkpoint.runtime.pregel->pending_inputs);
  }

  for (const auto &interrupt_id : checkpoint.resume_snapshot.interrupt_ids()) {
    auto location = checkpoint.resume_snapshot.location_of(interrupt_id);
    if (location.has_error()) {
      append_issue(validation, issue_kind::missing_resume_target, interrupt_id,
                   "checkpoint resume target has no address in the stored snapshot");
      continue;
    }
    auto path = strip_graph_root(location.value().get());
    if (!path.has_value() || path->empty() || !valid_paths.contains(*path)) {
      append_issue(validation, issue_kind::missing_resume_target,
                   location.value().get().to_string(),
                   "checkpoint resume target address no longer exists in the "
                   "current graph");
    }
  }

  for (const auto &[interrupt_id, location] :
       checkpoint.interrupt_snapshot.interrupt_id_to_address) {
    auto path = strip_graph_root(location);
    if (!path.has_value() || path->empty() || !valid_paths.contains(*path)) {
      append_issue(validation, issue_kind::missing_interrupt_target, location.to_string(),
                   "checkpoint interrupt target address no longer exists in "
                   "the current graph");
    }
  }

  return validation;
}

} // namespace wh::compose::restore_check
