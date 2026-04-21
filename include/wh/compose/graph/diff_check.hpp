// Defines compile-visible graph diff helpers used by public diff APIs.
#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "wh/compose/graph/snapshot.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::compose {

/// Typed diff category emitted by compile-visible graph comparison.
enum class graph_diff_kind : std::uint8_t {
  /// Graph compile-option field changed.
  compile_option = 0U,
  /// Node key exists only in candidate snapshot.
  node_added,
  /// Node key exists only in baseline snapshot.
  node_removed,
  /// Node keyed input/output signature changed.
  node_signature,
  /// Compile-visible node policy/contract/observation changed.
  node_policy,
  /// Edge exists only in candidate snapshot.
  edge_added,
  /// Edge exists only in baseline snapshot.
  edge_removed,
  /// Edge control/data routing shape changed.
  edge_topology,
  /// Edge adapter/limit policy changed.
  edge_policy,
  /// Branch declared destination set changed.
  branch_destinations,
};

/// One typed compile-visible graph diff entry.
struct graph_diff_entry {
  /// Diff category.
  graph_diff_kind kind{graph_diff_kind::compile_option};
  /// Subject path/key/edge label affected by this change.
  std::string subject{};
  /// Field/slot name within `subject`.
  std::string detail{};
  /// Previous compile-visible value.
  std::string before{};
  /// Candidate compile-visible value.
  std::string after{};
};

/// Ordered typed diff produced by one graph comparison.
struct graph_diff {
  /// Ordered diff entries.
  std::vector<graph_diff_entry> entries{};

  /// Returns entry count for one diff kind.
  [[nodiscard]] auto count(const graph_diff_kind kind) const noexcept -> std::size_t {
    return static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(),
        [kind](const graph_diff_entry &entry) -> bool { return entry.kind == kind; }));
  }

  /// Returns true when at least one entry of `kind` exists.
  [[nodiscard]] auto contains(const graph_diff_kind kind) const noexcept -> bool {
    return std::any_of(
        entries.begin(), entries.end(),
        [kind](const graph_diff_entry &entry) -> bool { return entry.kind == kind; });
  }

  /// Returns unique diff kinds in first-seen order.
  [[nodiscard]] auto kinds() const -> std::vector<graph_diff_kind> {
    std::vector<graph_diff_kind> unique{};
    unique.reserve(entries.size());
    for (const auto &entry : entries) {
      const auto iter = std::find(unique.begin(), unique.end(), entry.kind);
      if (iter == unique.end()) {
        unique.push_back(entry.kind);
      }
    }
    return unique;
  }
};

namespace detail {

struct graph_compatibility_report {
  graph_diff diff{};
  bool compatible{true};
  std::string message{};
};

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

[[nodiscard]] inline auto exec_mode_text(const node_exec_mode mode) -> std::string_view {
  return mode == node_exec_mode::async ? "async" : "sync";
}

[[nodiscard]] inline auto exec_origin_text(const node_exec_origin origin) -> std::string_view {
  return origin == node_exec_origin::lowered ? "lowered" : "authored";
}

[[nodiscard]] inline auto sync_dispatch_text(const sync_dispatch dispatch) -> std::string_view {
  return dispatch == sync_dispatch::inline_control ? "inline_control" : "work";
}

[[nodiscard]] inline auto size_text(const std::size_t value) -> std::string {
  return std::to_string(value);
}

[[nodiscard]] inline auto duration_text(const std::optional<std::chrono::milliseconds> &value)
    -> std::string {
  if (!value.has_value()) {
    return "none";
  }
  return std::to_string(value->count());
}

[[nodiscard]] inline auto optional_size_text(const std::optional<std::size_t> &value)
    -> std::string {
  if (!value.has_value()) {
    return "none";
  }
  return std::to_string(*value);
}

[[nodiscard]] inline auto optional_string_text(const std::optional<std::string> &value)
    -> std::string {
  if (!value.has_value()) {
    return "none";
  }
  return *value;
}

[[nodiscard]] inline auto join_strings(const std::vector<std::string> &values) -> std::string {
  std::string text{};
  for (std::size_t index = 0U; index < values.size(); ++index) {
    if (index > 0U) {
      text += ",";
    }
    text += values[index];
  }
  return text;
}

[[nodiscard]] inline auto graph_subject(const std::string_view prefix) -> std::string {
  if (prefix.empty()) {
    return "graph";
  }
  return std::string{prefix};
}

[[nodiscard]] inline auto join_subject(const std::string_view prefix, const std::string_view local)
    -> std::string {
  if (prefix.empty()) {
    return std::string{local};
  }
  std::string subject{prefix};
  subject.push_back('/');
  subject += local;
  return subject;
}

[[nodiscard]] inline auto edge_label(const std::string_view from, const std::string_view to)
    -> std::string {
  std::string label{from};
  label += "->";
  label += to;
  return label;
}

inline auto append_diff(graph_diff &diff, const graph_diff_kind kind, std::string subject,
                        std::string detail, std::string before, std::string after) -> void {
  diff.entries.push_back(graph_diff_entry{
      .kind = kind,
      .subject = std::move(subject),
      .detail = std::move(detail),
      .before = std::move(before),
      .after = std::move(after),
  });
}

inline auto mark_incompatible(bool &incompatible, std::string &message, std::string text) -> void {
  incompatible = true;
  if (message.empty()) {
    message = std::move(text);
  }
}

inline auto compare_node_options(graph_diff &diff, const std::string_view subject,
                                 const graph_snapshot_node &baseline,
                                 const graph_snapshot_node &candidate, bool &incompatible,
                                 std::string &message) -> void {
  const auto compare_policy = [&](const std::string_view detail, std::string before,
                                  std::string after, const bool breaking = false) -> void {
    if (before == after) {
      return;
    }
    append_diff(diff, graph_diff_kind::node_policy, std::string{subject}, std::string{detail},
                std::move(before), std::move(after));
    if (breaking) {
      mark_incompatible(incompatible, message,
                        "node contract changed at " + std::string{subject} + ":" +
                            std::string{detail});
    }
  };
  const auto compare_signature = [&](const std::string_view detail, std::string before,
                                     std::string after) -> void {
    if (before == after) {
      return;
    }
    append_diff(diff, graph_diff_kind::node_signature, std::string{subject}, std::string{detail},
                std::move(before), std::move(after));
    mark_incompatible(incompatible, message,
                      "node signature changed at " + std::string{subject} + ":" +
                          std::string{detail});
  };

  compare_signature("input_key", baseline.options.input_key, candidate.options.input_key);
  compare_signature("output_key", baseline.options.output_key, candidate.options.output_key);
  compare_policy("kind", std::string{node_kind_text(baseline.kind)},
                 std::string{node_kind_text(candidate.kind)});
  compare_policy("exec_mode", std::string{exec_mode_text(baseline.exec_mode)},
                 std::string{exec_mode_text(candidate.exec_mode)});
  compare_policy("exec_origin", std::string{exec_origin_text(baseline.exec_origin)},
                 std::string{exec_origin_text(candidate.exec_origin)});
  compare_policy("input_contract_kind", std::string{to_string(baseline.input_contract)},
                 std::string{to_string(candidate.input_contract)}, true);
  compare_policy("output_contract_kind", std::string{to_string(baseline.output_contract)},
                 std::string{to_string(candidate.output_contract)}, true);
  compare_policy("name", baseline.options.name, candidate.options.name);
  compare_policy("type", baseline.options.type, candidate.options.type);
  compare_policy("label", baseline.options.label, candidate.options.label);
  compare_policy("rerun_input_contract", std::to_string(baseline.options.input_contract),
                 std::to_string(candidate.options.input_contract));
  compare_policy("allow_no_control", bool_text(baseline.options.allow_no_control),
                 bool_text(candidate.options.allow_no_control));
  compare_policy("allow_no_data", bool_text(baseline.options.allow_no_data),
                 bool_text(candidate.options.allow_no_data));
  compare_policy("sync_dispatch", std::string{sync_dispatch_text(baseline.options.dispatch)},
                 std::string{sync_dispatch_text(candidate.options.dispatch)});
  compare_policy("callbacks_enabled", bool_text(baseline.options.observation.callbacks_enabled),
                 bool_text(candidate.options.observation.callbacks_enabled));
  compare_policy("allow_invoke_override",
                 bool_text(baseline.options.observation.allow_invoke_override),
                 bool_text(candidate.options.observation.allow_invoke_override));
  compare_policy("local_callback_count",
                 size_text(baseline.options.observation.local_callback_count),
                 size_text(candidate.options.observation.local_callback_count));
  compare_policy("retry_budget_override",
                 optional_size_text(baseline.options.retry_budget_override),
                 optional_size_text(candidate.options.retry_budget_override));
  compare_policy("timeout_override_ms", duration_text(baseline.options.timeout_override),
                 duration_text(candidate.options.timeout_override));
  compare_policy("retry_window_override_ms", duration_text(baseline.options.retry_window_override),
                 duration_text(candidate.options.retry_window_override));
  compare_policy("max_parallel_override",
                 optional_size_text(baseline.options.max_parallel_override),
                 optional_size_text(candidate.options.max_parallel_override));
  compare_policy("state_handlers.pre", bool_text(baseline.options.state_handlers.pre),
                 bool_text(candidate.options.state_handlers.pre));
  compare_policy("state_handlers.post", bool_text(baseline.options.state_handlers.post),
                 bool_text(candidate.options.state_handlers.post));
  compare_policy("state_handlers.stream_pre", bool_text(baseline.options.state_handlers.stream_pre),
                 bool_text(candidate.options.state_handlers.stream_pre));
  compare_policy("state_handlers.stream_post",
                 bool_text(baseline.options.state_handlers.stream_post),
                 bool_text(candidate.options.state_handlers.stream_post));
}

inline auto compare_graph_snapshots(graph_diff &diff, const graph_snapshot &baseline,
                                    const graph_snapshot &candidate, const std::string_view prefix,
                                    bool &incompatible, std::string &message) -> void {
  const auto graph_name = graph_subject(prefix);
  const auto compare_compile_option = [&](const std::string_view detail, std::string before,
                                          std::string after) -> void {
    if (before == after) {
      return;
    }
    append_diff(diff, graph_diff_kind::compile_option, graph_name, std::string{detail},
                std::move(before), std::move(after));
  };

  compare_compile_option("name", baseline.compile_options.name, candidate.compile_options.name);
  compare_compile_option("boundary",
                         std::string{to_string(baseline.compile_options.boundary.input)} + "->" +
                             std::string{to_string(baseline.compile_options.boundary.output)},
                         std::string{to_string(candidate.compile_options.boundary.input)} + "->" +
                             std::string{to_string(candidate.compile_options.boundary.output)});
  compare_compile_option("mode", std::string{mode_text(baseline.compile_options.mode)},
                         std::string{mode_text(candidate.compile_options.mode)});
  compare_compile_option(
      "dispatch_policy",
      std::string{dispatch_policy_text(baseline.compile_options.dispatch_policy)},
      std::string{dispatch_policy_text(candidate.compile_options.dispatch_policy)});
  compare_compile_option("max_steps", size_text(baseline.compile_options.max_steps),
                         size_text(candidate.compile_options.max_steps));
  compare_compile_option("retain_cold_data", bool_text(baseline.compile_options.retain_cold_data),
                         bool_text(candidate.compile_options.retain_cold_data));
  compare_compile_option("trigger_mode",
                         std::string{trigger_mode_text(baseline.compile_options.trigger_mode)},
                         std::string{trigger_mode_text(candidate.compile_options.trigger_mode)});
  compare_compile_option("fan_in_policy",
                         std::string{fan_in_policy_text(baseline.compile_options.fan_in_policy)},
                         std::string{fan_in_policy_text(candidate.compile_options.fan_in_policy)});
  compare_compile_option("retry_budget", size_text(baseline.compile_options.retry_budget),
                         size_text(candidate.compile_options.retry_budget));
  compare_compile_option("node_timeout_ms", duration_text(baseline.compile_options.node_timeout),
                         duration_text(candidate.compile_options.node_timeout));
  compare_compile_option("max_parallel_nodes",
                         size_text(baseline.compile_options.max_parallel_nodes),
                         size_text(candidate.compile_options.max_parallel_nodes));
  compare_compile_option("max_parallel_per_node",
                         size_text(baseline.compile_options.max_parallel_per_node),
                         size_text(candidate.compile_options.max_parallel_per_node));
  compare_compile_option("enable_local_state_generation",
                         bool_text(baseline.compile_options.enable_local_state_generation),
                         bool_text(candidate.compile_options.enable_local_state_generation));
  compare_compile_option("has_compile_callback",
                         bool_text(baseline.compile_options.has_compile_callback),
                         bool_text(candidate.compile_options.has_compile_callback));

  std::size_t baseline_node_index = 0U;
  std::size_t candidate_node_index = 0U;
  while (baseline_node_index < baseline.nodes.size() ||
         candidate_node_index < candidate.nodes.size()) {
    if (candidate_node_index == candidate.nodes.size() ||
        (baseline_node_index < baseline.nodes.size() &&
         baseline.nodes[baseline_node_index].key < candidate.nodes[candidate_node_index].key)) {
      append_diff(diff, graph_diff_kind::node_removed,
                  join_subject(prefix, baseline.nodes[baseline_node_index].key), "node", "present",
                  "absent");
      ++baseline_node_index;
      continue;
    }
    if (baseline_node_index == baseline.nodes.size() ||
        candidate.nodes[candidate_node_index].key < baseline.nodes[baseline_node_index].key) {
      append_diff(diff, graph_diff_kind::node_added,
                  join_subject(prefix, candidate.nodes[candidate_node_index].key), "node", "absent",
                  "present");
      ++candidate_node_index;
      continue;
    }

    const auto &baseline_node = baseline.nodes[baseline_node_index];
    const auto &candidate_node = candidate.nodes[candidate_node_index];
    const auto subject = join_subject(prefix, baseline_node.key);
    compare_node_options(diff, subject, baseline_node, candidate_node, incompatible, message);

    const auto baseline_subgraph = baseline.subgraphs.find(baseline_node.key);
    const auto candidate_subgraph = candidate.subgraphs.find(baseline_node.key);
    if (baseline_subgraph == baseline.subgraphs.end() &&
        candidate_subgraph != candidate.subgraphs.end()) {
      append_diff(diff, graph_diff_kind::node_policy, subject, "subgraph", "absent", "present");
    } else if (baseline_subgraph != baseline.subgraphs.end() &&
               candidate_subgraph == candidate.subgraphs.end()) {
      append_diff(diff, graph_diff_kind::node_policy, subject, "subgraph", "present", "absent");
    } else if (baseline_subgraph != baseline.subgraphs.end() &&
               candidate_subgraph != candidate.subgraphs.end()) {
      compare_graph_snapshots(diff, baseline_subgraph->second, candidate_subgraph->second, subject,
                              incompatible, message);
    }

    ++baseline_node_index;
    ++candidate_node_index;
  }

  auto baseline_edge_index = std::size_t{0U};
  auto candidate_edge_index = std::size_t{0U};
  while (baseline_edge_index < baseline.edges.size() ||
         candidate_edge_index < candidate.edges.size()) {
    const auto baseline_key = baseline_edge_index < baseline.edges.size()
                                  ? std::tie(baseline.edges[baseline_edge_index].from,
                                             baseline.edges[baseline_edge_index].to)
                                  : std::tie(candidate.edges[candidate_edge_index].from,
                                             candidate.edges[candidate_edge_index].to);
    const auto candidate_key = candidate_edge_index < candidate.edges.size()
                                   ? std::tie(candidate.edges[candidate_edge_index].from,
                                              candidate.edges[candidate_edge_index].to)
                                   : baseline_key;

    if (candidate_edge_index == candidate.edges.size() ||
        (baseline_edge_index < baseline.edges.size() && baseline_key < candidate_key)) {
      const auto &edge = baseline.edges[baseline_edge_index];
      append_diff(diff, graph_diff_kind::edge_removed,
                  join_subject(prefix, edge_label(edge.from, edge.to)), "edge", "present",
                  "absent");
      ++baseline_edge_index;
      continue;
    }
    if (baseline_edge_index == baseline.edges.size() || candidate_key < baseline_key) {
      const auto &edge = candidate.edges[candidate_edge_index];
      append_diff(diff, graph_diff_kind::edge_added,
                  join_subject(prefix, edge_label(edge.from, edge.to)), "edge", "absent",
                  "present");
      ++candidate_edge_index;
      continue;
    }

    const auto &baseline_edge = baseline.edges[baseline_edge_index];
    const auto &candidate_edge = candidate.edges[candidate_edge_index];
    const auto subject = join_subject(prefix, edge_label(baseline_edge.from, baseline_edge.to));
    const auto compare_edge_topology = [&](const std::string_view detail, std::string before,
                                           std::string after) -> void {
      if (before == after) {
        return;
      }
      append_diff(diff, graph_diff_kind::edge_topology, std::string{subject}, std::string{detail},
                  std::move(before), std::move(after));
    };
    const auto compare_edge_policy = [&](const std::string_view detail, std::string before,
                                         std::string after) -> void {
      if (before == after) {
        return;
      }
      append_diff(diff, graph_diff_kind::edge_policy, std::string{subject}, std::string{detail},
                  std::move(before), std::move(after));
    };

    compare_edge_topology("no_control", bool_text(baseline_edge.no_control),
                          bool_text(candidate_edge.no_control));
    compare_edge_topology("no_data", bool_text(baseline_edge.no_data),
                          bool_text(candidate_edge.no_data));
    compare_edge_policy("lowering_kind", std::string{to_string(baseline_edge.lowering_kind)},
                        std::string{to_string(candidate_edge.lowering_kind)});
    compare_edge_policy("has_custom_lowering", bool_text(baseline_edge.has_custom_lowering),
                        bool_text(candidate_edge.has_custom_lowering));
    compare_edge_policy("limits.max_items", size_text(baseline_edge.limits.max_items),
                        size_text(candidate_edge.limits.max_items));

    ++baseline_edge_index;
    ++candidate_edge_index;
  }

  auto baseline_branch_index = std::size_t{0U};
  auto candidate_branch_index = std::size_t{0U};
  while (baseline_branch_index < baseline.branches.size() ||
         candidate_branch_index < candidate.branches.size()) {
    if (candidate_branch_index == candidate.branches.size() ||
        (baseline_branch_index < baseline.branches.size() &&
         baseline.branches[baseline_branch_index].from <
             candidate.branches[candidate_branch_index].from)) {
      append_diff(diff, graph_diff_kind::branch_destinations,
                  join_subject(prefix, baseline.branches[baseline_branch_index].from),
                  "destinations", join_strings(baseline.branches[baseline_branch_index].end_nodes),
                  "absent");
      ++baseline_branch_index;
      continue;
    }
    if (baseline_branch_index == baseline.branches.size() ||
        candidate.branches[candidate_branch_index].from <
            baseline.branches[baseline_branch_index].from) {
      append_diff(diff, graph_diff_kind::branch_destinations,
                  join_subject(prefix, candidate.branches[candidate_branch_index].from),
                  "destinations", "absent",
                  join_strings(candidate.branches[candidate_branch_index].end_nodes));
      ++candidate_branch_index;
      continue;
    }

    const auto &baseline_branch = baseline.branches[baseline_branch_index];
    const auto &candidate_branch = candidate.branches[candidate_branch_index];
    if (baseline_branch.end_nodes != candidate_branch.end_nodes) {
      append_diff(diff, graph_diff_kind::branch_destinations,
                  join_subject(prefix, baseline_branch.from), "destinations",
                  join_strings(baseline_branch.end_nodes),
                  join_strings(candidate_branch.end_nodes));
    }
    ++baseline_branch_index;
    ++candidate_branch_index;
  }
}

[[nodiscard]] inline auto compare_graph_snapshots(const graph_snapshot &baseline,
                                                  const graph_snapshot &candidate)
    -> graph_compatibility_report {
  graph_compatibility_report report{};
  compare_graph_snapshots(report.diff, baseline, candidate, "", report.compatible = true,
                          report.message);
  return report;
}

} // namespace detail

/// Returns the compile-visible diff between two graph snapshots.
[[nodiscard]] inline auto diff_graph(const graph_snapshot &baseline,
                                     const graph_snapshot &candidate) -> graph_diff {
  return detail::compare_graph_snapshots(baseline, candidate).diff;
}

} // namespace wh::compose
