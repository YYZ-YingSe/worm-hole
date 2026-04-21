// Defines workflow authored builder that lowers step-local dependencies into
// graph.
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

#include "wh/compose/authored/parallel.hpp"
#include "wh/compose/authored/stream_branch.hpp"
#include "wh/compose/authored/value_branch.hpp"
#include "wh/compose/field.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/compose/payload/map.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"

namespace wh::compose {

/// Stable internal key prefix used by lowered workflow input nodes.
inline constexpr std::string_view workflow_input_node_key_prefix = "__wf_input__";

/// Returns the stable internal pre-node key used for one workflow target.
[[nodiscard]] inline auto workflow_input_node_key(const std::string_view target_key)
    -> std::string {
  std::string key{workflow_input_node_key_prefix};
  key.append(target_key);
  return key;
}

/// Node-centric workflow authoring facade that lowers to one plain `graph`.
///
/// This surface follows the same core shape as Eino's compose workflow:
/// steps own their incoming dependencies and input mappings, while `workflow`
/// only stages authoring and lowers the result into one real graph. There is no
/// second workflow runtime; the compiled form is always a lowered `graph`.
class workflow {
  struct input_node_plan {
    std::string target_key{};
    std::vector<compiled_field_mapping_rule> rules{};
    bool has_incoming_edge{false};
  };

  struct control_edge {
    std::string from{};
    std::string to{};
  };

  using control_edge_list = std::vector<control_edge>;

  template <typename branch_t> struct branch_plan {
    std::string from{};
    branch_t branch{};
  };

public:
  /// Step-local handle used to author dependencies and static inputs.
  class step_ref {
  public:
    step_ref() = delete;

    /// Returns the authored step key referenced by this handle.
    [[nodiscard]] auto key() const noexcept -> std::string_view { return key_; }

    /// Adds one graph-entry input dependency to this step.
    auto from_entry() const -> wh::core::result<void> {
      return add_dependency_from_key(graph_start_node_key, workflow_dependency_kind::control_data,
                                     {});
    }

    /// Adds one execution-only dependency from the graph entry boundary.
    auto from_entry_without_input() const -> wh::core::result<void> {
      return add_dependency_from_key(graph_start_node_key, workflow_dependency_kind::control, {});
    }

    /// Adds one mapped graph-entry input dependency to this step.
    auto from_entry(std::vector<field_mapping_rule> mappings) const -> wh::core::result<void> {
      return add_dependency_from_key(graph_start_node_key, workflow_dependency_kind::control_data,
                                     std::move(mappings));
    }

    /// Adds one execution-only dependency from another step.
    auto add_dependency(const step_ref &from) const -> wh::core::result<void> {
      auto valid = validate();
      if (valid.has_error()) {
        return valid;
      }
      auto from_valid = validate_source_ref(from);
      if (from_valid.has_error()) {
        return from_valid;
      }
      return add_dependency_from_key(from.key_, workflow_dependency_kind::control, {});
    }

    /// Adds one data+control input dependency from another step.
    auto add_input(const step_ref &from) const -> wh::core::result<void> {
      auto valid = validate();
      if (valid.has_error()) {
        return valid;
      }
      auto from_valid = validate_source_ref(from);
      if (from_valid.has_error()) {
        return from_valid;
      }
      return add_dependency_from_key(from.key_, workflow_dependency_kind::control_data, {});
    }

    /// Adds one mapped data+control input dependency from another step.
    auto add_input(const step_ref &from, std::vector<field_mapping_rule> mappings) const
        -> wh::core::result<void> {
      auto valid = validate();
      if (valid.has_error()) {
        return valid;
      }
      auto from_valid = validate_source_ref(from);
      if (from_valid.has_error()) {
        return from_valid;
      }
      return add_dependency_from_key(from.key_, workflow_dependency_kind::control_data,
                                     std::move(mappings));
    }

    /// Adds one data-only input dependency from another step.
    auto add_input_without_control(const step_ref &from) const -> wh::core::result<void> {
      auto valid = validate();
      if (valid.has_error()) {
        return valid;
      }
      auto from_valid = validate_source_ref(from);
      if (from_valid.has_error()) {
        return from_valid;
      }
      return add_dependency_from_key(from.key_, workflow_dependency_kind::data, {});
    }

    /// Adds one mapped data-only input dependency from another step.
    auto add_input_without_control(const step_ref &from,
                                   std::vector<field_mapping_rule> mappings) const
        -> wh::core::result<void> {
      auto valid = validate();
      if (valid.has_error()) {
        return valid;
      }
      auto from_valid = validate_source_ref(from);
      if (from_valid.has_error()) {
        return from_valid;
      }
      return add_dependency_from_key(from.key_, workflow_dependency_kind::data,
                                     std::move(mappings));
    }

    /// Adds one value branch routed from this step.
    auto add_value_branch(const value_branch &branch) const -> wh::core::result<void> {
      auto source_valid = validate_source();
      if (source_valid.has_error()) {
        return source_valid;
      }
      return owner_->stage_value_branch(key_, branch);
    }

    /// Adds one value branch routed from this step.
    auto add_value_branch(value_branch &&branch) const -> wh::core::result<void> {
      auto source_valid = validate_source();
      if (source_valid.has_error()) {
        return source_valid;
      }
      return owner_->stage_value_branch(key_, std::move(branch));
    }

    /// Adds one stream branch routed from this step.
    auto add_stream_branch(const stream_branch &branch) const -> wh::core::result<void> {
      auto source_valid = validate_source();
      if (source_valid.has_error()) {
        return source_valid;
      }
      return owner_->stage_stream_branch(key_, branch);
    }

    /// Adds one stream branch routed from this step.
    auto add_stream_branch(stream_branch &&branch) const -> wh::core::result<void> {
      auto source_valid = validate_source();
      if (source_valid.has_error()) {
        return source_valid;
      }
      return owner_->stage_stream_branch(key_, std::move(branch));
    }

    /// Expands one authored parallel group from this step.
    auto add_parallel(const parallel &group) const -> wh::core::result<std::vector<step_ref>> {
      auto source_valid = validate_source();
      if (source_valid.has_error()) {
        return wh::core::result<std::vector<step_ref>>::failure(source_valid.error());
      }
      return owner_->add_parallel_from(key_, group);
    }

    /// Expands one authored parallel group from this step.
    auto add_parallel(parallel &&group) const -> wh::core::result<std::vector<step_ref>> {
      auto source_valid = validate_source();
      if (source_valid.has_error()) {
        return wh::core::result<std::vector<step_ref>>::failure(source_valid.error());
      }
      return owner_->add_parallel_from(key_, std::move(group));
    }

    /// Adds one static value visible at this step's input boundary.
    template <typename path_t, typename value_t>
      requires std::constructible_from<std::string, path_t &&> &&
               std::constructible_from<graph_value, value_t &&>
    auto set_static_value(path_t &&path, value_t &&value) const -> wh::core::result<void> {
      auto valid = validate();
      if (valid.has_error()) {
        return valid;
      }
      field_mapping_rule rule{};
      rule.to_path = std::string{std::forward<path_t>(path)};
      rule.static_value = graph_value{std::forward<value_t>(value)};
      return owner_->add_static_value(key_, std::move(rule));
    }

  private:
    friend class workflow;

    explicit step_ref(workflow *owner, std::string key) : owner_(owner), key_(std::move(key)) {}

    [[nodiscard]] auto validate() const -> wh::core::result<void> {
      return owner_->validate_target_key(key_);
    }

    [[nodiscard]] auto validate_source() const -> wh::core::result<void> {
      return owner_->validate_source_key(key_);
    }

    [[nodiscard]] auto validate_source_ref(const step_ref &from) const -> wh::core::result<void> {
      if (from.owner_ != owner_) {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
      return owner_->validate_source_key(from.key_);
    }

    auto add_dependency_from_key(const std::string_view from, const workflow_dependency_kind kind,
                                 std::vector<field_mapping_rule> mappings) const
        -> wh::core::result<void> {
      workflow_dependency dependency{};
      dependency.from = std::string{from};
      dependency.to = key_;
      dependency.kind = kind;
      dependency.mappings = std::move(mappings);
      return owner_->stage_dependency(std::move(dependency));
    }

    workflow *owner_{nullptr};
    std::string key_{};
  };

  workflow() = default;
  explicit workflow(const graph_compile_options &options) : graph_(options) {}
  explicit workflow(graph_compile_options &&options) : graph_(std::move(options)) {}

  template <authored_node_like node_t>
  /// Registers one authored node as a workflow step and returns a step handle.
  auto add_step(node_t &&node) -> wh::core::result<step_ref> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return wh::core::result<step_ref>::failure(writable.error());
    }

    const auto step_key = std::string{node.key()};
    wh::core::result<void> status{};
    using stored_t = std::remove_cvref_t<node_t>;
    if constexpr (std::same_as<stored_t, component_node>) {
      status = graph_.add_component(std::forward<node_t>(node));
    } else if constexpr (std::same_as<stored_t, lambda_node>) {
      status = graph_.add_lambda(std::forward<node_t>(node));
    } else if constexpr (std::same_as<stored_t, subgraph_node>) {
      status = graph_.add_subgraph(std::forward<node_t>(node));
    } else if constexpr (std::same_as<stored_t, tools_node>) {
      status = graph_.add_tools(std::forward<node_t>(node));
    } else {
      status = graph_.add_passthrough(std::forward<node_t>(node));
    }
    return retain_step(step_key, status);
  }

  template <typename key_t, typename graph_t, typename options_t = graph_add_node_options>
    requires std::constructible_from<std::string, key_t &&> &&
             std::constructible_from<graph_add_node_options, options_t &&> &&
             graph_viewable<std::remove_cvref_t<graph_t>>
  /// Registers one graph-like object as a workflow step via a subgraph node.
  auto add_subgraph_step(key_t &&key, graph_t &&subgraph, options_t &&options = {})
      -> wh::core::result<step_ref> {
    return add_step(make_subgraph_node(std::forward<key_t>(key), std::forward<graph_t>(subgraph),
                                       std::forward<options_t>(options)));
  }

  /// Returns a handle to one previously authored step.
  auto step(const std::string_view key) -> wh::core::result<step_ref> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return wh::core::result<step_ref>::failure(writable.error());
    }
    if (key == graph_end_node_key) {
      return step_ref{this, std::string{graph_end_node_key}};
    }
    if (!authored_steps_.contains(key)) {
      return wh::core::result<step_ref>::failure(wh::core::errc::not_found);
    }
    return step_ref{this, std::string{key}};
  }

  /// Returns a handle representing the workflow `END` boundary.
  [[nodiscard]] auto end() noexcept -> step_ref {
    return step_ref{this, std::string{graph_end_node_key}};
  }

  /// Lowers staged workflow authoring into the owned graph and freezes
  /// mutations.
  auto compile() -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }

    auto lowered = lower_authoring();
    if (lowered.has_error()) {
      return lowered;
    }

    auto compiled = graph_.compile();
    if (compiled.has_error()) {
      return retain_error(compiled.error());
    }

    dependencies_.clear();
    static_rules_.clear();
    validation_buckets_.clear();
    value_branches_.clear();
    stream_branches_.clear();
    compiled_ = true;
    return {};
  }

  template <typename input_t>
    requires std::constructible_from<graph_value_map, input_t &&>
  /// Executes the lowered graph and returns one sender of
  /// `result<graph_value_map>`.
  [[nodiscard]] auto invoke(wh::core::run_context &context, input_t &&input) const {
    using result_t = wh::core::result<graph_value_map>;
    using error_sender_t = wh::core::detail::ready_sender_t<result_t>;
    using request_t = wh::compose::graph_invoke_request;
    using graph_sender_t = std::remove_cvref_t<decltype(map_graph_output_sender(
        graph_.invoke(context,
                      request_t{.input = wh::compose::graph_input::value(value_map_to_payload(
                                    graph_value_map{std::forward<input_t>(input)}))}),
        output_source_keys_))>;
    using sender_t = wh::core::detail::variant_sender<error_sender_t, graph_sender_t>;

    if (!compiled_ || first_error_.has_value()) {
      return sender_t{wh::core::detail::failure_result_sender<result_t>(
          first_error_.value_or(wh::core::errc::contract_violation))};
    }

    return sender_t{map_graph_output_sender(
        graph_.invoke(context,
                      request_t{.input = wh::compose::graph_input::value(value_map_to_payload(
                                    graph_value_map{std::forward<input_t>(input)}))}),
        output_source_keys_)};
  }

  /// Returns the lowered graph view.
  [[nodiscard]] auto graph_view() const noexcept -> const graph & { return graph_; }

  /// Releases the lowered graph for nesting or ownership transfer.
  [[nodiscard]] auto release_graph() && noexcept -> graph { return std::move(graph_); }

private:
  [[nodiscard]] static constexpr auto
  has_control_dependency(const workflow_dependency_kind kind) noexcept -> bool {
    return kind == workflow_dependency_kind::control ||
           kind == workflow_dependency_kind::control_data;
  }

  [[nodiscard]] static constexpr auto
  has_data_dependency(const workflow_dependency_kind kind) noexcept -> bool {
    return kind == workflow_dependency_kind::data || kind == workflow_dependency_kind::control_data;
  }

  [[nodiscard]] static constexpr auto
  carries_graph_input(const std::string_view source_key) noexcept -> bool {
    return source_key == graph_start_node_key;
  }

  [[nodiscard]] static constexpr auto
  dependency_carries_input_payload(const workflow_dependency &dependency) noexcept -> bool {
    return has_data_dependency(dependency.kind) || carries_graph_input(dependency.from);
  }

  [[nodiscard]] static auto map_graph_output_sender(stdexec::sender auto &&sender) {
    return map_graph_output_sender(std::forward<decltype(sender)>(sender), {});
  }

  [[nodiscard]] static auto
  map_graph_output_sender(stdexec::sender auto &&sender,
                          const std::vector<std::string> &output_source_keys) {
    using result_t = wh::core::result<graph_value_map>;
    return wh::core::detail::map_result_sender<result_t>(
        wh::core::detail::normalize_result_sender<wh::core::result<graph_invoke_result>>(
            std::forward<decltype(sender)>(sender)),
        [output_source_keys](graph_invoke_result invoke_result) {
          if (invoke_result.output_status.has_error()) {
            return wh::core::result<graph_value_map>::failure(
                std::move(invoke_result.output_status).error());
          }
          auto output = payload_to_value_map(std::move(invoke_result.output_status).value());
          if (output.has_error()) {
            return output;
          }

          if (output.value().size() != 1U) {
            return output;
          }

          const auto iter = output.value().begin();
          if (std::ranges::find(output_source_keys, iter->first) == output_source_keys.end()) {
            return output;
          }

          auto unwrapped = payload_to_value_map(graph_value{iter->second});
          if (unwrapped.has_error()) {
            return wh::core::result<graph_value_map>::failure(unwrapped.error());
          }
          return unwrapped;
        });
  }

  [[nodiscard]] static auto validate_dependency(const workflow_dependency &dependency)
      -> wh::core::result<void> {
    if (dependency.from.empty() || dependency.to.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (dependency.from == "*" && dependency.to == "*") {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!has_data_dependency(dependency.kind) && !dependency.mappings.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (has_data_dependency(dependency.kind) && !has_control_dependency(dependency.kind) &&
        dependency.mappings.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    return {};
  }

  [[nodiscard]] static auto has_control_path(const control_edge_list &control_edges,
                                             const std::string_view from, const std::string_view to)
      -> bool {
    if (from == to) {
      return true;
    }

    std::unordered_map<std::string_view, std::vector<std::string_view>> adjacency{};
    for (const auto &edge : control_edges) {
      adjacency[edge.from].push_back(edge.to);
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

  [[nodiscard]] static auto is_path_prefix(const field_path &lhs, const field_path &rhs) -> bool {
    if (lhs.segments.size() > rhs.segments.size()) {
      return false;
    }
    return std::ranges::equal(lhs.segments, rhs.segments | std::views::take(lhs.segments.size()));
  }

  [[nodiscard]] static auto is_whole_object_mapping(const field_mapping_rule &rule) -> bool {
    return rule.to_path == "*";
  }

  [[nodiscard]] static auto
  validate_mapping_rule(const field_mapping_rule &rule,
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
    if (std::ranges::any_of(existing_rules, [](const field_mapping_rule &existing) -> bool {
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
      -> wh::core::result<step_ref> {
    if (status.has_error()) {
      return wh::core::result<step_ref>::failure(status.error());
    }
    step_keys_.push_back(step_key);
    authored_steps_.emplace(step_key);
    validation_buckets_.try_emplace(step_key);
    static_rules_.try_emplace(step_key);
    return step_ref{this, step_key};
  }

  [[nodiscard]] auto validate_source_key(const std::string_view key) const
      -> wh::core::result<void> {
    if (key == graph_start_node_key) {
      return {};
    }
    if (!authored_steps_.contains(key)) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return {};
  }

  [[nodiscard]] auto validate_target_key(const std::string_view key) const
      -> wh::core::result<void> {
    if (key == graph_end_node_key) {
      return {};
    }
    if (!authored_steps_.contains(key)) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return {};
  }

  auto add_static_value(const std::string_view target, field_mapping_rule rule)
      -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    auto target_valid = validate_target_key(target);
    if (target_valid.has_error()) {
      return retain_error(target_valid.error());
    }

    auto &bucket = validation_buckets_[std::string{target}];
    auto validated = validate_mapping_rule(rule, bucket);
    if (validated.has_error()) {
      return retain_error(validated.error());
    }
    auto compiled_rule = compile_field_mapping_rule(rule);
    if (compiled_rule.has_error()) {
      return retain_error(compiled_rule.error());
    }

    bucket.push_back(rule);
    static_rules_[std::string{target}].push_back(std::move(rule));
    return {};
  }

  auto stage_dependency(workflow_dependency dependency) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }

    auto validated = validate_dependency(dependency);
    if (validated.has_error()) {
      return retain_error(validated.error());
    }
    auto source_valid = validate_source_key(dependency.from);
    if (source_valid.has_error()) {
      return retain_error(source_valid.error());
    }
    auto target_valid = validate_target_key(dependency.to);
    if (target_valid.has_error()) {
      return retain_error(target_valid.error());
    }
    auto rules_valid = validate_staged_rules(dependency.to, dependency);
    if (rules_valid.has_error()) {
      return retain_error(rules_valid.error());
    }

    dependencies_.push_back(std::move(dependency));
    return {};
  }

  auto stage_value_branch(const std::string_view from, const value_branch &branch)
      -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    auto source_valid = validate_source_key(from);
    if (source_valid.has_error()) {
      return retain_error(source_valid.error());
    }
    value_branches_.push_back(branch_plan<value_branch>{
        .from = std::string{from},
        .branch = branch,
    });
    return {};
  }

  auto stage_value_branch(const std::string_view from, value_branch &&branch)
      -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    auto source_valid = validate_source_key(from);
    if (source_valid.has_error()) {
      return retain_error(source_valid.error());
    }
    value_branches_.push_back(branch_plan<value_branch>{
        .from = std::string{from},
        .branch = std::move(branch),
    });
    return {};
  }

  auto stage_stream_branch(const std::string_view from, const stream_branch &branch)
      -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    auto source_valid = validate_source_key(from);
    if (source_valid.has_error()) {
      return retain_error(source_valid.error());
    }
    stream_branches_.push_back(branch_plan<stream_branch>{
        .from = std::string{from},
        .branch = branch,
    });
    return {};
  }

  auto stage_stream_branch(const std::string_view from, stream_branch &&branch)
      -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    auto source_valid = validate_source_key(from);
    if (source_valid.has_error()) {
      return retain_error(source_valid.error());
    }
    stream_branches_.push_back(branch_plan<stream_branch>{
        .from = std::string{from},
        .branch = std::move(branch),
    });
    return {};
  }

  template <typename group_t>
  auto add_parallel_from(const std::string_view from, group_t &&group)
      -> wh::core::result<std::vector<step_ref>> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return wh::core::result<std::vector<step_ref>>::failure(writable.error());
    }
    auto source_valid = validate_source_key(from);
    if (source_valid.has_error()) {
      return wh::core::result<std::vector<step_ref>>::failure(source_valid.error());
    }

    using stored_group_t = std::remove_cvref_t<group_t>;
    stored_group_t stored_group{std::forward<group_t>(group)};
    const auto &nodes = stored_group.nodes();
    if (nodes.size() < 2U) {
      return wh::core::result<std::vector<step_ref>>::failure(wh::core::errc::invalid_argument);
    }

    std::vector<step_ref> added_steps{};
    added_steps.reserve(nodes.size());
    for (auto node : nodes) {
      auto added = std::visit(
          [this](auto &value) -> wh::core::result<step_ref> {
            return this->add_step(std::move(value));
          },
          node);
      if (added.has_error()) {
        return wh::core::result<std::vector<step_ref>>::failure(added.error());
      }
      auto linked = added.value().add_input(step_ref{this, std::string{from}});
      if (linked.has_error()) {
        return wh::core::result<std::vector<step_ref>>::failure(linked.error());
      }
      added_steps.push_back(std::move(added).value());
    }
    return added_steps;
  }

  auto validate_staged_rules(const std::string_view target, const workflow_dependency &dependency)
      -> wh::core::result<void> {
    if (!dependency_carries_input_payload(dependency)) {
      return {};
    }

    if (dependency.mappings.empty()) {
      return {};
    }

    auto &bucket = validation_buckets_[std::string{target}];
    for (const auto &rule : dependency.mappings) {
      auto validated = validate_mapping_rule(rule, bucket);
      if (validated.has_error()) {
        return validated;
      }
      auto compiled_rule = compile_field_mapping_rule(rule);
      if (compiled_rule.has_error()) {
        return wh::core::result<void>::failure(compiled_rule.error());
      }
      bucket.push_back(rule);
    }
    return {};
  }

  [[nodiscard]] static auto find_or_add_input_plan(
      std::vector<input_node_plan> &plans,
      std::unordered_map<std::string, std::size_t, wh::core::transparent_string_hash,
                         wh::core::transparent_string_equal> &plan_index,
      const std::string_view target_key) -> input_node_plan & {
    const auto [iter, inserted] = plan_index.emplace(target_key, plans.size());
    if (inserted) {
      plans.push_back(input_node_plan{.target_key = std::string{target_key}});
    }
    return plans[iter->second];
  }

  [[nodiscard]] static auto make_input_node(const std::string &key,
                                            std::vector<compiled_field_mapping_rule> rules)
      -> lambda_node {
    graph_add_node_options options{};
    options.type = "workflow.input";
    options.label = "workflow.input";
    return make_lambda_node(
        key,
        [rules = std::move(rules)](graph_value_map &input, wh::core::run_context &context,
                                   const graph_call_scope &) -> wh::core::result<graph_value_map> {
          auto updated = apply_field_mappings_in_place(input, rules, context);
          if (updated.has_error()) {
            return wh::core::result<graph_value_map>::failure(updated.error());
          }
          return std::move(input);
        },
        std::move(options));
  }

  [[nodiscard]] auto collect_control_edges() const -> control_edge_list {
    control_edge_list control_edges{};
    control_edges.reserve(dependencies_.size() + value_branches_.size() + stream_branches_.size());
    for (const auto &dependency : dependencies_) {
      if (has_control_dependency(dependency.kind)) {
        control_edges.push_back(control_edge{.from = dependency.from, .to = dependency.to});
      }
    }
    for (const auto &branch : value_branches_) {
      for (const auto &target : branch.branch.end_nodes()) {
        control_edges.push_back(control_edge{.from = branch.from, .to = target});
      }
    }
    for (const auto &branch : stream_branches_) {
      for (const auto &target : branch.branch.end_nodes()) {
        control_edges.push_back(control_edge{.from = branch.from, .to = target});
      }
    }
    return control_edges;
  }

  [[nodiscard]] auto terminal_steps(const control_edge_list &control_edges) const
      -> std::vector<std::string> {
    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        non_terminal_steps{};
    for (const auto &[from, to] : control_edges) {
      static_cast<void>(to);
      non_terminal_steps.emplace(from);
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

  [[nodiscard]] static auto has_explicit_end_route(const control_edge_list &control_edges) noexcept
      -> bool {
    return std::ranges::any_of(
        control_edges, [](const auto &edge) noexcept { return edge.to == graph_end_node_key; });
  }

  auto validate_terminal_shape(const control_edge_list &control_edges) -> wh::core::result<void> {
    if (has_explicit_end_route(control_edges)) {
      return {};
    }

    auto terminals = terminal_steps(control_edges);
    if (terminals.empty()) {
      return retain_error(wh::core::errc::contract_violation);
    }
    if (terminals.size() != 1U) {
      return retain_error(wh::core::errc::contract_violation);
    }
    return {};
  }

  [[nodiscard]] auto determine_input_node_targets() const
      -> std::unordered_set<std::string, wh::core::transparent_string_hash,
                            wh::core::transparent_string_equal> {
    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        targets{};
    for (const auto &dependency : dependencies_) {
      if (dependency.mappings.empty()) {
        continue;
      }
      targets.emplace(dependency.to);
    }
    for (const auto &[target, rules] : static_rules_) {
      if (rules.empty()) {
        continue;
      }
      targets.emplace(target);
    }
    return targets;
  }

  [[nodiscard]] static auto rewrite_target_key(
      const std::string_view target,
      const std::unordered_set<std::string, wh::core::transparent_string_hash,
                               wh::core::transparent_string_equal> &input_node_targets)
      -> std::string {
    if (input_node_targets.contains(target)) {
      return workflow_input_node_key(target);
    }
    return std::string{target};
  }

  [[nodiscard]] static auto build_input_plans(
      const std::vector<workflow_dependency> &dependencies,
      const std::unordered_map<std::string, std::vector<field_mapping_rule>,
                               wh::core::transparent_string_hash,
                               wh::core::transparent_string_equal> &static_rules,
      const std::unordered_set<std::string, wh::core::transparent_string_hash,
                               wh::core::transparent_string_equal> &input_node_targets)
      -> wh::core::result<std::vector<input_node_plan>> {
    std::vector<input_node_plan> plans{};
    std::unordered_map<std::string, std::size_t, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        plan_index{};

    for (const auto &target : input_node_targets) {
      static_cast<void>(find_or_add_input_plan(plans, plan_index, target));
    }

    for (const auto &dependency : dependencies) {
      if (!input_node_targets.contains(dependency.to) || dependency.mappings.empty()) {
        continue;
      }
      auto &plan = find_or_add_input_plan(plans, plan_index, dependency.to);
      for (const auto &rule : dependency.mappings) {
        auto compiled_rule = compile_field_mapping_rule(rule);
        if (compiled_rule.has_error()) {
          return wh::core::result<std::vector<input_node_plan>>::failure(compiled_rule.error());
        }
        plan.rules.push_back(std::move(compiled_rule).value());
      }
    }

    for (const auto &[target, rules] : static_rules) {
      if (!input_node_targets.contains(target)) {
        continue;
      }
      auto &plan = find_or_add_input_plan(plans, plan_index, target);
      for (const auto &rule : rules) {
        auto compiled_rule = compile_field_mapping_rule(rule);
        if (compiled_rule.has_error()) {
          return wh::core::result<std::vector<input_node_plan>>::failure(compiled_rule.error());
        }
        plan.rules.push_back(std::move(compiled_rule).value());
      }
    }

    return plans;
  }

  auto add_input_nodes(const std::vector<input_node_plan> &plans) -> wh::core::result<void> {
    for (const auto &plan : plans) {
      auto added =
          graph_.add_lambda(make_input_node(workflow_input_node_key(plan.target_key), plan.rules));
      if (added.has_error()) {
        return retain_error(added.error());
      }
    }
    return {};
  }

  [[nodiscard]] static auto build_input_plan_lookup(std::vector<input_node_plan> &plans)
      -> std::unordered_map<std::string, input_node_plan *, wh::core::transparent_string_hash,
                            wh::core::transparent_string_equal> {
    std::unordered_map<std::string, input_node_plan *, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        lookup{};
    lookup.reserve(plans.size());
    for (auto &plan : plans) {
      lookup.emplace(plan.target_key, std::addressof(plan));
    }
    return lookup;
  }

  auto lower_dependency_edges(
      const control_edge_list &control_edges,
      const std::unordered_set<std::string, wh::core::transparent_string_hash,
                               wh::core::transparent_string_equal> &input_node_targets,
      std::unordered_map<std::string, input_node_plan *, wh::core::transparent_string_hash,
                         wh::core::transparent_string_equal> &plan_lookup)
      -> wh::core::result<void> {
    for (const auto &dependency : dependencies_) {
      if (has_data_dependency(dependency.kind) && !has_control_dependency(dependency.kind) &&
          !has_control_path(control_edges, dependency.from, dependency.to)) {
        return retain_error(wh::core::errc::contract_violation);
      }

      const auto destination = rewrite_target_key(dependency.to, input_node_targets);
      if (auto iter = plan_lookup.find(dependency.to); iter != plan_lookup.end()) {
        iter->second->has_incoming_edge = true;
      }

      auto linked = graph_.add_edge(dependency.from, destination,
                                    edge_options{
                                        .no_control = !has_control_dependency(dependency.kind),
                                        .no_data = !dependency_carries_input_payload(dependency),
                                    });
      if (linked.has_error()) {
        return retain_error(linked.error());
      }
    }
    return {};
  }

  auto apply_value_branches(
      const std::unordered_set<std::string, wh::core::transparent_string_hash,
                               wh::core::transparent_string_equal> &input_node_targets,
      std::unordered_map<std::string, input_node_plan *, wh::core::transparent_string_hash,
                         wh::core::transparent_string_equal> &plan_lookup)
      -> wh::core::result<void> {
    for (const auto &plan : value_branches_) {
      auto source_valid = validate_source_key(plan.from);
      if (source_valid.has_error()) {
        return retain_error(source_valid.error());
      }

      value_branch rewritten{};
      for (const auto &entry : plan.branch.cases()) {
        auto target_valid = validate_target_key(entry.to);
        if (target_valid.has_error()) {
          return retain_error(target_valid.error());
        }
        if (auto iter = plan_lookup.find(entry.to); iter != plan_lookup.end()) {
          iter->second->has_incoming_edge = true;
        }
        auto added =
            rewritten.add_case(rewrite_target_key(entry.to, input_node_targets), entry.match);
        if (added.has_error()) {
          return retain_error(added.error());
        }
      }

      auto applied = std::move(rewritten).apply(graph_, plan.from);
      if (applied.has_error()) {
        return retain_error(applied.error());
      }
    }
    return {};
  }

  auto apply_stream_branches(
      const std::unordered_set<std::string, wh::core::transparent_string_hash,
                               wh::core::transparent_string_equal> &input_node_targets,
      std::unordered_map<std::string, input_node_plan *, wh::core::transparent_string_hash,
                         wh::core::transparent_string_equal> &plan_lookup)
      -> wh::core::result<void> {
    for (const auto &plan : stream_branches_) {
      auto source_valid = validate_source_key(plan.from);
      if (source_valid.has_error()) {
        return retain_error(source_valid.error());
      }

      stream_branch rewritten{};
      std::unordered_map<std::string, std::string, wh::core::transparent_string_hash,
                         wh::core::transparent_string_equal>
          target_rewrites{};
      target_rewrites.reserve(plan.branch.targets().size());

      for (const auto &target : plan.branch.targets()) {
        auto target_valid = validate_target_key(target);
        if (target_valid.has_error()) {
          return retain_error(target_valid.error());
        }
        if (auto iter = plan_lookup.find(target); iter != plan_lookup.end()) {
          iter->second->has_incoming_edge = true;
        }
        auto rewritten_target = rewrite_target_key(target, input_node_targets);
        target_rewrites.emplace(target, rewritten_target);
        auto added = rewritten.add_target(rewritten_target);
        if (added.has_error()) {
          return retain_error(added.error());
        }
      }

      if (plan.branch.selector()) {
        auto selected = rewritten.set_selector(
            [selector = plan.branch.selector(), target_rewrites = std::move(target_rewrites)](
                graph_stream_reader input,
                wh::core::run_context &context) -> wh::core::result<std::vector<std::string>> {
              auto routed = selector(std::move(input), context);
              if (routed.has_error()) {
                return wh::core::result<std::vector<std::string>>::failure(routed.error());
              }
              std::vector<std::string> rewritten_keys{};
              rewritten_keys.reserve(routed.value().size());
              for (const auto &key : routed.value()) {
                const auto iter = target_rewrites.find(key);
                if (iter == target_rewrites.end()) {
                  return wh::core::result<std::vector<std::string>>::failure(
                      wh::core::errc::contract_violation);
                }
                rewritten_keys.push_back(iter->second);
              }
              return rewritten_keys;
            });
        if (selected.has_error()) {
          return retain_error(selected.error());
        }
      }

      auto applied = std::move(rewritten).apply(graph_, plan.from);
      if (applied.has_error()) {
        return retain_error(applied.error());
      }
    }
    return {};
  }

  auto connect_input_nodes(std::vector<input_node_plan> &plans) -> wh::core::result<void> {
    for (auto &plan : plans) {
      const auto input_key = workflow_input_node_key(plan.target_key);
      if (!plan.has_incoming_edge) {
        auto linked = graph_.add_entry_edge(input_key, edge_options{.no_data = false});
        if (linked.has_error()) {
          return retain_error(linked.error());
        }
      }

      auto linked = graph_.add_edge(input_key, plan.target_key);
      if (linked.has_error()) {
        return retain_error(linked.error());
      }
    }
    return {};
  }

  [[nodiscard]] auto collect_output_source_keys(
      const control_edge_list &control_edges,
      const std::unordered_set<std::string, wh::core::transparent_string_hash,
                               wh::core::transparent_string_equal> &input_node_targets) const
      -> std::vector<std::string> {
    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        keys{};
    auto terminals = terminal_steps(control_edges);
    keys.reserve(terminals.size() + dependencies_.size() + value_branches_.size() +
                 stream_branches_.size() + 1U);
    for (auto &terminal : terminals) {
      keys.insert(std::move(terminal));
    }

    if (input_node_targets.contains(graph_end_node_key)) {
      keys.emplace(workflow_input_node_key(graph_end_node_key));
    } else {
      for (const auto &dependency : dependencies_) {
        if (dependency.to != graph_end_node_key || !dependency_carries_input_payload(dependency)) {
          continue;
        }
        keys.emplace(dependency.from);
      }
      for (const auto &branch : value_branches_) {
        if (!std::ranges::any_of(branch.branch.cases(),
                                 [](const value_branch_case &entry) noexcept -> bool {
                                   return entry.to == graph_end_node_key;
                                 })) {
          continue;
        }
        keys.emplace(branch.from);
      }
      for (const auto &branch : stream_branches_) {
        if (!std::ranges::any_of(branch.branch.targets(),
                                 [](const std::string &target) noexcept -> bool {
                                   return target == graph_end_node_key;
                                 })) {
          continue;
        }
        keys.emplace(branch.from);
      }
    }

    std::vector<std::string> output_sources{};
    output_sources.reserve(keys.size());
    for (auto &key : keys) {
      output_sources.push_back(std::move(key));
    }
    return output_sources;
  }

  auto connect_terminals_to_end(const control_edge_list &control_edges) -> wh::core::result<void> {
    auto terminals = terminal_steps(control_edges);
    for (const auto &step_key : terminals) {
      auto linked = graph_.add_exit_edge(step_key);
      if (linked.has_error() && linked.error() != wh::core::errc::already_exists) {
        return retain_error(linked.error());
      }
    }
    return {};
  }

  auto lower_authoring() -> wh::core::result<void> {
    const auto control_edges = collect_control_edges();
    auto valid_terminals = validate_terminal_shape(control_edges);
    if (valid_terminals.has_error()) {
      return valid_terminals;
    }

    const auto input_node_targets = determine_input_node_targets();
    auto plans = build_input_plans(dependencies_, static_rules_, input_node_targets);
    if (plans.has_error()) {
      return retain_error(plans.error());
    }

    auto added_input_nodes = add_input_nodes(plans.value());
    if (added_input_nodes.has_error()) {
      return added_input_nodes;
    }

    auto plan_lookup = build_input_plan_lookup(plans.value());

    auto lowered_dependencies =
        lower_dependency_edges(control_edges, input_node_targets, plan_lookup);
    if (lowered_dependencies.has_error()) {
      return lowered_dependencies;
    }

    auto lowered_value_branches = apply_value_branches(input_node_targets, plan_lookup);
    if (lowered_value_branches.has_error()) {
      return lowered_value_branches;
    }

    auto lowered_stream_branches = apply_stream_branches(input_node_targets, plan_lookup);
    if (lowered_stream_branches.has_error()) {
      return lowered_stream_branches;
    }

    output_source_keys_ = collect_output_source_keys(control_edges, input_node_targets);

    auto connected_inputs = connect_input_nodes(plans.value());
    if (connected_inputs.has_error()) {
      return connected_inputs;
    }

    return connect_terminals_to_end(control_edges);
  }

  /// Owned lowered graph artifact.
  graph graph_{};
  /// Registered authored workflow step keys in insertion order.
  std::vector<std::string> step_keys_{};
  /// Fast lookup set for authored workflow step keys.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      authored_steps_{};
  /// Authored source keys that may legitimately project workflow output.
  std::vector<std::string> output_source_keys_{};
  /// Staged workflow dependency declarations before lowering.
  std::vector<workflow_dependency> dependencies_{};
  /// Staged target-local static mapping rules.
  std::unordered_map<std::string, std::vector<field_mapping_rule>,
                     wh::core::transparent_string_hash, wh::core::transparent_string_equal>
      static_rules_{};
  /// Per-target staged mapping validation bucket.
  std::unordered_map<std::string, std::vector<field_mapping_rule>,
                     wh::core::transparent_string_hash, wh::core::transparent_string_equal>
      validation_buckets_{};
  /// Staged value-branch authored declarations before lowering.
  std::vector<branch_plan<value_branch>> value_branches_{};
  /// Staged stream-branch authored declarations before lowering.
  std::vector<branch_plan<stream_branch>> stream_branches_{};
  /// Sticky first error used for fail-fast authoring semantics.
  std::optional<wh::core::error_code> first_error_{};
  /// True once lowering + graph compile succeeded.
  bool compiled_{false};
};

} // namespace wh::compose
