// Defines workflow builder that lowers dependency mapping into a real graph.
#pragma once

#include <algorithm>
#include <concepts>
#include <optional>
#include <queue>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/field.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/payload/map.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"

namespace wh::compose {

/// Map-shaped authoring facade that lowers workflow dependencies into `graph`.
///
/// `workflow` stays responsible for authoring `step + dependency + mapping`
/// semantics, but its compiled form is a plain lowered `graph`. That lets one
/// compiled workflow participate in subgraph nesting and snapshot/diff/restore
/// flows without keeping a separate runtime-only mapping layer alive.
class workflow {
  struct mapping_plan {
    std::string target_key{};
    std::vector<std::string> gating_sources{};
    std::vector<compiled_field_mapping_rule> rules{};
  };

public:
  workflow() = default;
  explicit workflow(const graph_compile_options &options) : graph_(options) {}
  explicit workflow(graph_compile_options &&options) : graph_(std::move(options)) {}

  template <authored_node_like node_t>
  /// Registers one authored node as a workflow step.
  auto add_step(node_t &&node) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }

    const auto step_key = std::string{node.key()};
    using stored_t = std::remove_cvref_t<node_t>;
    if constexpr (std::same_as<stored_t, component_node>) {
      return retain_step(step_key,
                         graph_.add_component(std::forward<node_t>(node)));
    } else if constexpr (std::same_as<stored_t, lambda_node>) {
      return retain_step(step_key, graph_.add_lambda(std::forward<node_t>(node)));
    } else if constexpr (std::same_as<stored_t, subgraph_node>) {
      return retain_step(step_key, graph_.add_subgraph(std::forward<node_t>(node)));
    } else if constexpr (std::same_as<stored_t, tools_node>) {
      return retain_step(step_key, graph_.add_tools(std::forward<node_t>(node)));
    } else {
      return retain_step(step_key,
                         graph_.add_passthrough(std::forward<node_t>(node)));
    }
  }

  /// Registers one workflow dependency declaration.
  auto add_dependency(const workflow_dependency &dependency)
      -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    dependencies_.push_back(dependency);
    return {};
  }

  /// Registers one workflow dependency declaration.
  auto add_dependency(workflow_dependency &&dependency) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    dependencies_.push_back(std::move(dependency));
    return {};
  }

  /// Lowers workflow dependencies into the owned graph and freezes mutations.
  auto compile() -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }

    auto lowered = lower_dependencies();
    if (lowered.has_error()) {
      return lowered;
    }

    auto compiled = graph_.compile();
    if (compiled.has_error()) {
      return retain_error(compiled.error());
    }

    dependencies_.clear();
    compiled_ = true;
    return {};
  }

  template <typename input_t>
    requires std::constructible_from<graph_value_map, input_t &&>
  /// Executes the lowered graph and returns one sender of `result<graph_value_map>`.
  [[nodiscard]] auto invoke(wh::core::run_context &context, input_t &&input) const {
    using result_t = wh::core::result<graph_value_map>;
    using error_sender_t = wh::core::detail::ready_sender_t<result_t>;
    using request_t = wh::compose::graph_invoke_request;
    using graph_sender_t =
        std::remove_cvref_t<decltype(map_graph_output_sender(graph_.invoke(
            context, request_t{.input = value_map_to_payload(
                         graph_value_map{std::forward<input_t>(input)})})))>;
    using sender_t = wh::core::detail::variant_sender<error_sender_t, graph_sender_t>;

    if (!compiled_ || first_error_.has_value()) {
      return sender_t{wh::core::detail::failure_result_sender<result_t>(
          first_error_.value_or(wh::core::errc::contract_violation))};
    }

    return sender_t{map_graph_output_sender(graph_.invoke(
        context, request_t{.input = value_map_to_payload(
                     graph_value_map{std::forward<input_t>(input)})}))};
  }

  /// Returns the lowered graph view.
  [[nodiscard]] auto graph_view() const noexcept -> const graph & { return graph_; }

  /// Releases the lowered graph for nesting or ownership transfer.
  [[nodiscard]] auto release_graph() && noexcept -> graph {
    return std::move(graph_);
  }

private:
  [[nodiscard]] static constexpr auto
  has_control_dependency(const workflow_dependency_kind kind) noexcept -> bool {
    return kind == workflow_dependency_kind::control ||
           kind == workflow_dependency_kind::control_data;
  }

  [[nodiscard]] static constexpr auto
  has_data_dependency(const workflow_dependency_kind kind) noexcept -> bool {
    return kind == workflow_dependency_kind::data ||
           kind == workflow_dependency_kind::control_data;
  }

  [[nodiscard]] static auto make_mapping_node_key(const std::string_view target_key)
      -> std::string {
    std::string key{"__wf_map__"};
    key.append(target_key);
    return key;
  }

  [[nodiscard]] static auto map_graph_output_sender(stdexec::sender auto &&sender) {
    using result_t = wh::core::result<graph_value_map>;
    return wh::core::detail::map_result_sender<result_t>(
        wh::core::detail::normalize_result_sender<
            wh::core::result<graph_invoke_result>>(
            std::forward<decltype(sender)>(sender)),
        [](graph_invoke_result invoke_result) {
          if (invoke_result.output_status.has_error()) {
            return wh::core::result<graph_value_map>::failure(
                std::move(invoke_result.output_status).error());
          }
          return payload_to_value_map(
              std::move(invoke_result.output_status).value());
        });
  }

  [[nodiscard]] static auto validate_dependency(
      const workflow_dependency &dependency) -> wh::core::result<void> {
    if (dependency.from.empty() || dependency.to.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (dependency.from == "*" && dependency.to == "*") {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!has_data_dependency(dependency.kind) && !dependency.mappings.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (has_data_dependency(dependency.kind) &&
        !has_control_dependency(dependency.kind) && dependency.mappings.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    return {};
  }

  [[nodiscard]] static auto has_control_path(
      const std::vector<std::pair<std::string_view, std::string_view>> &control_edges,
      const std::string_view from, const std::string_view to) -> bool {
    if (from == to) {
      return true;
    }

    std::unordered_map<std::string_view, std::vector<std::string_view>> adjacency{};
    for (const auto &edge : control_edges) {
      adjacency[edge.first].push_back(edge.second);
    }

    std::queue<std::string_view> queue{};
    std::unordered_set<std::string_view> visited{};
    queue.push(from);
    visited.insert(from);
    while (!queue.empty()) {
      const auto current = queue.front();
      queue.pop();
      const auto iter = adjacency.find(current);
      if (iter == adjacency.end()) {
        continue;
      }
      for (const auto &next : iter->second) {
        if (next == to) {
          return true;
        }
        if (!visited.insert(next).second) {
          continue;
        }
        queue.push(next);
      }
    }
    return false;
  }

  [[nodiscard]] static auto is_path_prefix(const field_path &lhs,
                                           const field_path &rhs) -> bool {
    if (lhs.segments.size() > rhs.segments.size()) {
      return false;
    }
    return std::ranges::equal(
        lhs.segments, rhs.segments | std::views::take(lhs.segments.size()));
  }

  [[nodiscard]] static auto is_whole_object_mapping(
      const field_mapping_rule &rule) -> bool {
    return rule.to_path == "*";
  }

  [[nodiscard]] static auto validate_mapping_rule(
      const field_mapping_rule &rule,
      const std::vector<field_mapping_rule> &existing_rules)
      -> wh::core::result<void> {
    if (rule.to_path.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (rule.static_value.has_value()) {
      if (!rule.from_path.empty() || static_cast<bool>(rule.extractor)) {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
    } else if (!rule.extractor && rule.from_path.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    const bool new_whole_mapping = is_whole_object_mapping(rule);
    if (new_whole_mapping && !existing_rules.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (std::ranges::any_of(existing_rules,
                            [](const field_mapping_rule &existing) -> bool {
                              return is_whole_object_mapping(existing);
                            })) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }

    if (new_whole_mapping) {
      return {};
    }

    auto new_to_path = parse_field_path(rule.to_path);
    if (new_to_path.has_error()) {
      return wh::core::result<void>::failure(new_to_path.error());
    }
    if (!rule.static_value.has_value() && !rule.extractor) {
      auto parsed_from = parse_field_path(rule.from_path);
      if (parsed_from.has_error()) {
        return wh::core::result<void>::failure(parsed_from.error());
      }
    }

    for (const auto &existing : existing_rules) {
      auto existing_to_path = parse_field_path(existing.to_path);
      if (existing_to_path.has_error()) {
        return wh::core::result<void>::failure(existing_to_path.error());
      }
      if (is_path_prefix(new_to_path.value(), existing_to_path.value()) ||
          is_path_prefix(existing_to_path.value(), new_to_path.value())) {
        return wh::core::result<void>::failure(wh::core::errc::already_exists);
      }
    }
    return {};
  }

  [[nodiscard]] auto ensure_writable() const -> wh::core::result<void> {
    if (compiled_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (first_error_.has_value()) {
      return wh::core::result<void>::failure(*first_error_);
    }
    return {};
  }

  auto retain_error(const wh::core::error_code code) -> wh::core::result<void> {
    if (!first_error_.has_value()) {
      first_error_ = code;
    }
    return wh::core::result<void>::failure(code);
  }

  auto retain_step(const std::string &step_key, const wh::core::result<void> &status)
      -> wh::core::result<void> {
    if (status.has_error()) {
      return retain_error(status.error());
    }
    step_keys_.push_back(step_key);
    return {};
  }

  [[nodiscard]] static auto find_or_add_plan(
      std::vector<mapping_plan> &plans,
      std::unordered_map<std::string, std::size_t, wh::core::transparent_string_hash,
                         wh::core::transparent_string_equal> &plan_index,
      const std::string_view target_key) -> mapping_plan & {
    const auto [iter, inserted] = plan_index.emplace(target_key, plans.size());
    if (inserted) {
      plans.push_back(mapping_plan{.target_key = std::string{target_key}});
    }
    return plans[iter->second];
  }

  [[nodiscard]] static auto make_mapping_node(
      const std::string &key, std::vector<compiled_field_mapping_rule> rules)
      -> lambda_node {
    graph_add_node_options options{};
    options.type = "workflow.map";
    options.label = "workflow.map";
    return make_lambda_node(
        key,
        [rules = std::move(rules)](const graph_value_map &input,
                                   wh::core::run_context &context,
                                   const graph_call_scope &)
            -> wh::core::result<graph_value_map> {
          graph_value_map mapped = input;
          auto updated = apply_field_mappings_in_place(mapped, rules, context);
          if (updated.has_error()) {
            return wh::core::result<graph_value_map>::failure(updated.error());
          }
          return mapped;
        },
        std::move(options));
  }

  auto lower_mapping_plans(std::vector<mapping_plan> plans)
      -> wh::core::result<void> {
    for (auto &plan : plans) {
      auto mapping_key = make_mapping_node_key(plan.target_key);
      auto added = graph_.add_lambda(
          make_mapping_node(mapping_key, std::move(plan.rules)));
      if (added.has_error()) {
        return retain_error(added.error());
      }

      if (plan.gating_sources.empty()) {
        auto linked = graph_.add_entry_edge(mapping_key, edge_options{.no_data = true});
        if (linked.has_error()) {
          return retain_error(linked.error());
        }
      } else {
        for (const auto &source : plan.gating_sources) {
          auto linked = graph_.add_edge(source, mapping_key,
                                        edge_options{.no_data = true});
          if (linked.has_error()) {
            return retain_error(linked.error());
          }
        }
      }

      auto linked = graph_.add_edge(mapping_key, plan.target_key);
      if (linked.has_error()) {
        return retain_error(linked.error());
      }
    }
    return {};
  }

  [[nodiscard]] auto terminal_steps() const -> std::vector<std::string> {
    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        non_terminal_steps{};
    for (const auto &dependency : dependencies_) {
      if (has_control_dependency(dependency.kind)) {
        non_terminal_steps.emplace(dependency.from);
      }
    }

    std::vector<std::string> terminals{};
    terminals.reserve(step_keys_.size());
    for (const auto &step_key : step_keys_) {
      if (!non_terminal_steps.contains(step_key)) {
        terminals.push_back(step_key);
      }
    }
    return terminals;
  }

  auto validate_terminal_shape() -> wh::core::result<void> {
    auto terminals = terminal_steps();
    if (terminals.empty()) {
      return retain_error(wh::core::errc::contract_violation);
    }
    if (terminals.size() != 1U) {
      return retain_error(wh::core::errc::contract_violation);
    }
    return {};
  }

  auto connect_terminals_to_end() -> wh::core::result<void> {
    auto terminals = terminal_steps();
    for (const auto &step_key : terminals) {
      auto linked = graph_.add_exit_edge(step_key);
      if (linked.has_error() && linked.error() != wh::core::errc::already_exists) {
        return retain_error(linked.error());
      }
    }
    return {};
  }

  auto lower_dependencies() -> wh::core::result<void> {
    std::vector<std::pair<std::string_view, std::string_view>> control_edges{};
    control_edges.reserve(dependencies_.size());
    for (const auto &dependency : dependencies_) {
      auto validated = validate_dependency(dependency);
      if (validated.has_error()) {
        return retain_error(validated.error());
      }
      if (has_control_dependency(dependency.kind)) {
        control_edges.emplace_back(dependency.from, dependency.to);
      }
    }

    std::vector<mapping_plan> mapping_plans{};
    std::unordered_map<std::string, std::size_t, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        mapping_plan_index{};
    std::unordered_map<std::string, std::vector<field_mapping_rule>,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        validation_buckets{};

    for (const auto &dependency : dependencies_) {
      if (has_data_dependency(dependency.kind) &&
          !has_control_dependency(dependency.kind) &&
          !has_control_path(control_edges, dependency.from, dependency.to)) {
        return retain_error(wh::core::errc::contract_violation);
      }

      if (!dependency.mappings.empty()) {
        auto &plan =
            find_or_add_plan(mapping_plans, mapping_plan_index, dependency.to);
        auto &validation_bucket = validation_buckets[dependency.to];
        if (has_control_dependency(dependency.kind) &&
            std::ranges::find(plan.gating_sources, dependency.from) ==
                plan.gating_sources.end()) {
          plan.gating_sources.push_back(dependency.from);
        }
        for (const auto &rule : dependency.mappings) {
          auto validated = validate_mapping_rule(rule, validation_bucket);
          if (validated.has_error()) {
            return retain_error(validated.error());
          }
          auto compiled_rule = compile_field_mapping_rule(rule);
          if (compiled_rule.has_error()) {
            return retain_error(compiled_rule.error());
          }
          validation_bucket.push_back(rule);
          plan.rules.push_back(std::move(compiled_rule).value());
        }
        continue;
      }

      auto linked = graph_.add_edge(
          dependency.from, dependency.to,
          edge_options{
              .no_control = !has_control_dependency(dependency.kind),
              .no_data = !has_data_dependency(dependency.kind),
          });
      if (linked.has_error()) {
        return retain_error(linked.error());
      }
    }

    auto valid_terminals = validate_terminal_shape();
    if (valid_terminals.has_error()) {
      return valid_terminals;
    }

    auto lowered_mappings = lower_mapping_plans(std::move(mapping_plans));
    if (lowered_mappings.has_error()) {
      return lowered_mappings;
    }
    return connect_terminals_to_end();
  }

  /// Owned lowered graph artifact.
  graph graph_{};
  /// Registered authored workflow step keys in insertion order.
  std::vector<std::string> step_keys_{};
  /// Staged workflow dependency declarations before lowering.
  std::vector<workflow_dependency> dependencies_{};
  /// Sticky first error used for fail-fast authoring semantics.
  std::optional<wh::core::error_code> first_error_{};
  /// True once lowering + graph compile succeeded.
  bool compiled_{false};
};

} // namespace wh::compose
