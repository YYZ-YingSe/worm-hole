// Defines out-of-line graph builder and mutation helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

namespace detail {

[[nodiscard]] inline auto tools_node_required_endpoint(const node_exec_mode exec_mode,
                                                       const node_contract output_contract) noexcept
    -> std::string_view {
  if (output_contract == node_contract::stream) {
    return exec_mode == node_exec_mode::async ? "async_stream" : "stream";
  }
  return exec_mode == node_exec_mode::async ? "async_invoke" : "invoke";
}

[[nodiscard]] inline auto tools_entry_matches_endpoint(const tool_entry &entry,
                                                       const node_exec_mode exec_mode,
                                                       const node_contract output_contract) noexcept
    -> bool {
  if (output_contract == node_contract::stream) {
    return exec_mode == node_exec_mode::async ? static_cast<bool>(entry.async_stream)
                                              : static_cast<bool>(entry.stream);
  }
  return exec_mode == node_exec_mode::async ? static_cast<bool>(entry.async_invoke)
                                            : static_cast<bool>(entry.invoke);
}

[[nodiscard]] inline auto validate_tools_node_entrypoints(const std::string_view node_key,
                                                          const node_descriptor &descriptor,
                                                          const tools_payload &payload)
    -> std::optional<std::string> {
  if (descriptor.exec_mode == node_exec_mode::sync && !payload.runtime_options.sequential) {
    return "tools node `" + std::string{node_key} +
           "` does not support non-sequential sync execution";
  }
  const auto required =
      tools_node_required_endpoint(descriptor.exec_mode, descriptor.output_contract);
  for (const auto &[tool_name, entry] : payload.registry) {
    if (tools_entry_matches_endpoint(entry, descriptor.exec_mode, descriptor.output_contract)) {
      continue;
    }
    return "tools node `" + std::string{node_key} + "` requires `" + std::string{required} +
           "` for tool `" + tool_name + "`";
  }
  if (payload.runtime_options.missing.has_value() &&
      !tools_entry_matches_endpoint(*payload.runtime_options.missing, descriptor.exec_mode,
                                    descriptor.output_contract)) {
    return "tools node `" + std::string{node_key} + "` requires `" + std::string{required} +
           "` for missing-tool entry";
  }
  return std::nullopt;
}

} // namespace detail

inline auto graph::add_component(const component_node &node) -> wh::core::result<void> {
  return add_node_impl(node);
}

inline auto graph::add_component(component_node &&node) -> wh::core::result<void> {
  return add_node_impl(std::move(node));
}

inline auto graph::add_lambda(const lambda_node &node) -> wh::core::result<void> {
  return add_node_impl(node);
}

inline auto graph::add_lambda(lambda_node &&node) -> wh::core::result<void> {
  return add_node_impl(std::move(node));
}

inline auto graph::add_subgraph(const subgraph_node &node) -> wh::core::result<void> {
  return add_node_impl(node);
}

inline auto graph::add_subgraph(subgraph_node &&node) -> wh::core::result<void> {
  return add_node_impl(std::move(node));
}

inline auto graph::add_tools(const tools_node &node) -> wh::core::result<void> {
  return add_node_impl(node);
}

inline auto graph::add_tools(tools_node &&node) -> wh::core::result<void> {
  return add_node_impl(std::move(node));
}

inline auto graph::add_passthrough(const passthrough_node &node) -> wh::core::result<void> {
  return add_node_impl(node);
}

inline auto graph::add_passthrough(passthrough_node &&node) -> wh::core::result<void> {
  return add_node_impl(std::move(node));
}

template <node_contract From, node_contract To, node_exec_mode Exec, typename key_t,
          typename lambda_t, typename options_t>
inline auto graph::add_lambda(key_t &&key, lambda_t &&lambda, options_t &&options)
    -> wh::core::result<void> {
  return add_lambda(make_lambda_node<From, To, Exec>(
      std::forward<key_t>(key), std::forward<lambda_t>(lambda), std::forward<options_t>(options)));
}

template <component_kind Kind, node_contract From, node_contract To, node_exec_mode Exec,
          typename key_t, typename component_t, typename options_t>
  requires(Kind != component_kind::custom)
inline auto graph::add_component(key_t &&key, component_t &&component, options_t &&options)
    -> wh::core::result<void> {
  return add_component(make_component_node<Kind, From, To, Exec>(
      std::forward<key_t>(key), std::forward<component_t>(component),
      std::forward<options_t>(options)));
}

template <typename key_t, typename graph_t, typename options_t>
  requires graph_viewable<std::remove_cvref_t<graph_t>>
inline auto graph::add_subgraph(key_t &&key, graph_t &&subgraph, options_t &&options)
    -> wh::core::result<void> {
  return add_subgraph(make_subgraph_node(std::forward<key_t>(key), std::forward<graph_t>(subgraph),
                                         std::forward<options_t>(options)));
}

template <node_contract From, node_contract To, node_exec_mode Exec, typename key_t,
          typename registry_t, typename options_t, typename tool_options_t>
inline auto graph::add_tools(key_t &&key, registry_t &&registry, options_t &&options,
                             tool_options_t &&tool_options) -> wh::core::result<void> {
  return add_tools(make_tools_node<From, To, Exec>(
      std::forward<key_t>(key), std::forward<registry_t>(registry),
      std::forward<options_t>(options), std::forward<tool_options_t>(tool_options)));
}

template <node_contract Contract, typename key_t>
inline auto graph::add_passthrough(key_t &&key) -> wh::core::result<void> {
  return add_passthrough(make_passthrough_node<Contract>(std::forward<key_t>(key)));
}

inline auto graph::add_edge(const graph_edge &edge) -> wh::core::result<void> {
  return add_edge_impl(edge);
}

inline auto graph::add_edge(graph_edge &&edge) -> wh::core::result<void> {
  return add_edge_impl(std::move(edge));
}

template <typename from_t, typename to_t>
  requires std::constructible_from<std::string, from_t &&> &&
           std::constructible_from<std::string, to_t &&>
inline auto graph::add_edge(from_t &&from, to_t &&to, edge_options options)
    -> wh::core::result<void> {
  return add_edge(graph_edge{
      .from = std::forward<from_t>(from),
      .to = std::forward<to_t>(to),
      .options = std::move(options),
  });
}

template <typename to_t>
  requires std::constructible_from<std::string, to_t &&>
inline auto graph::add_entry_edge(to_t &&to, edge_options options) -> wh::core::result<void> {
  return add_edge(std::string{graph_start_node_key}, std::forward<to_t>(to), std::move(options));
}

template <typename from_t>
  requires std::constructible_from<std::string, from_t &&>
inline auto graph::add_exit_edge(from_t &&from, edge_options options) -> wh::core::result<void> {
  return add_edge(std::forward<from_t>(from), std::string{graph_end_node_key}, std::move(options));
}

inline auto graph::add_value_branch(const graph_value_branch &branch) -> wh::core::result<void> {
  return add_value_branch_impl(branch);
}

inline auto graph::add_value_branch(graph_value_branch &&branch) -> wh::core::result<void> {
  return add_value_branch_impl(std::move(branch));
}

inline auto graph::add_stream_branch(const graph_stream_branch &branch) -> wh::core::result<void> {
  return add_stream_branch_impl(branch);
}

inline auto graph::add_stream_branch(graph_stream_branch &&branch) -> wh::core::result<void> {
  return add_stream_branch_impl(std::move(branch));
}

template <typename node_t>
inline auto graph::add_node_impl(node_t &&node) -> wh::core::result<void> {
  auto writable = ensure_writable();
  if (writable.has_error()) {
    return writable;
  }

  const auto node_key_view = authored_key(node);
  if (node_key_view.empty()) {
    return fail_fast(wh::core::errc::invalid_argument, "node key is empty");
  }
  if (core().nodes_.contains(node_key_view)) {
    return fail_fast(wh::core::errc::already_exists,
                     "node already exists: " + std::string{node_key_view});
  }
  if constexpr (std::same_as<std::remove_cvref_t<node_t>, tools_node>) {
    if (auto failure =
            detail::validate_tools_node_entrypoints(node_key_view, node.descriptor_, node.payload_);
        failure.has_value()) {
      return fail_fast(wh::core::errc::not_supported, std::move(*failure));
    }
  }

  const auto next_node_id = static_cast<std::uint32_t>(core().node_insertion_order_.size());
  if constexpr (std::is_lvalue_reference_v<node_t>) {
    auto stored_key = std::string{node_key_view};
    core().node_insertion_order_.push_back(stored_key);
    core().node_id_index_.emplace(stored_key, next_node_id);
    core().nodes_.emplace(std::move(stored_key), authored_node{node});
  } else {
    auto node_key = std::string{node_key_view};
    core().node_insertion_order_.push_back(node_key);
    core().node_id_index_.emplace(node_key, next_node_id);
    core().nodes_.emplace(std::move(node_key), authored_node{std::move(node)});
  }
  return {};
}

inline constexpr auto graph::default_edge_lowering_kind(const node_contract source_output,
                                                        const node_contract target_input) noexcept
    -> edge_lowering_kind {
  if (source_output == target_input) {
    return edge_lowering_kind::none;
  }
  if (source_output == node_contract::value && target_input == node_contract::stream) {
    return edge_lowering_kind::value_to_stream;
  }
  if (source_output == node_contract::stream && target_input == node_contract::value) {
    return edge_lowering_kind::stream_to_value;
  }
  return edge_lowering_kind::custom;
}

inline auto graph::resolve_edge_adapter(const graph_edge &edge) const
    -> wh::core::result<edge_adapter> {
  const auto source_iter = core().nodes_.find(edge.from);
  const auto target_iter = core().nodes_.find(edge.to);
  if (source_iter == core().nodes_.end() || target_iter == core().nodes_.end()) {
    return wh::core::result<edge_adapter>::failure(wh::core::errc::not_found);
  }

  const auto source_output = authored_output_contract(source_iter->second);
  const auto target_input = authored_input_contract(target_iter->second);
  auto adapter = edge.options.adapter;
  const auto required = default_edge_lowering_kind(source_output, target_input);

  if (required == edge_lowering_kind::none) {
    if (adapter.kind != edge_adapter_kind::none) {
      return wh::core::result<edge_adapter>::failure(wh::core::errc::contract_violation);
    }
    return adapter;
  }

  if (adapter.kind == edge_adapter_kind::none) {
    return adapter;
  }

  if (adapter.kind == edge_adapter_kind::custom) {
    if (required == edge_lowering_kind::value_to_stream) {
      if (!adapter.custom.to_stream.has_value()) {
        return wh::core::result<edge_adapter>::failure(wh::core::errc::contract_violation);
      }
      return adapter;
    }
    if (required == edge_lowering_kind::stream_to_value) {
      if (!adapter.custom.to_value.has_value()) {
        return wh::core::result<edge_adapter>::failure(wh::core::errc::contract_violation);
      }
      return adapter;
    }
    return wh::core::result<edge_adapter>::failure(wh::core::errc::contract_violation);
  }

  return wh::core::result<edge_adapter>::failure(wh::core::errc::contract_violation);
}

template <typename edge_t>
inline auto graph::add_edge_impl(edge_t &&edge) -> wh::core::result<void> {
  auto writable = ensure_writable();
  if (writable.has_error()) {
    return writable;
  }

  if (edge.from.empty() || edge.to.empty()) {
    return fail_fast(wh::core::errc::invalid_argument, "edge endpoint key is empty");
  }
  if (edge.options.no_control && edge.options.no_data) {
    return fail_fast(wh::core::errc::invalid_argument,
                     "edge noControl and noData cannot both be true");
  }
  if (!core().nodes_.contains(edge.from) || !core().nodes_.contains(edge.to)) {
    return fail_fast(wh::core::errc::not_found,
                     "edge endpoint not found: " + edge.from + "->" + edge.to);
  }
  if (has_edge(edge.from, edge.to)) {
    return fail_fast(wh::core::errc::already_exists,
                     "duplicate edge: " + edge.from + "->" + edge.to);
  }
  auto adapter = resolve_edge_adapter(edge);
  if (adapter.has_error()) {
    return fail_fast(wh::core::errc::contract_violation,
                     "edge contract mismatch: " + edge.from + "->" + edge.to);
  }

  graph_edge normalized_edge{};
  normalized_edge.from = std::string{edge.from};
  normalized_edge.to = std::string{edge.to};
  if constexpr (std::is_lvalue_reference_v<edge_t>) {
    normalized_edge.options = edge.options;
  } else {
    normalized_edge.options = std::move(edge.options);
  }
  normalized_edge.options.adapter = std::move(adapter).value();

  core().edges_.push_back(std::move(normalized_edge));
  return {};
}

template <typename branch_t>
inline auto graph::add_value_branch_impl(branch_t &&branch) -> wh::core::result<void> {
  auto writable = ensure_writable();
  if (writable.has_error()) {
    return writable;
  }
  if (branch.from.empty()) {
    return fail_fast(wh::core::errc::invalid_argument, "value-branch source key is empty");
  }
  if (!core().nodes_.contains(branch.from)) {
    return fail_fast(wh::core::errc::not_found,
                     "value-branch source node not found: " + branch.from);
  }
  if (core().value_branches_.contains(branch.from) ||
      core().stream_branches_.contains(branch.from)) {
    return fail_fast(wh::core::errc::already_exists,
                     "branch source already registered: " + branch.from);
  }
  if (branch.end_nodes.empty()) {
    return fail_fast(wh::core::errc::invalid_argument, "value-branch end-nodes are empty");
  }

  const auto source_output = authored_output_contract(core().nodes_.at(branch.from));
  if (source_output != node_contract::value) {
    return fail_fast(wh::core::errc::contract_violation,
                     "value-branch source must produce value output: " + branch.from);
  }

  std::unordered_set<std::string_view, detail::string_hash, detail::string_equal>
      unique_end_nodes{};
  unique_end_nodes.reserve(branch.end_nodes.size());
  for (const auto &to : branch.end_nodes) {
    if (!core().nodes_.contains(to)) {
      return fail_fast(wh::core::errc::not_found, "branch end-node not found: " + to);
    }
    if (!unique_end_nodes.insert(to).second) {
      return fail_fast(wh::core::errc::already_exists, "value-branch end-node duplicated: " + to);
    }
    auto added = add_edge(branch.from, to);
    if (added.has_error()) {
      return added;
    }
  }

  value_branch_definition definition{};
  if constexpr (std::is_lvalue_reference_v<branch_t>) {
    definition.end_nodes = branch.end_nodes;
    definition.selector_ids = branch.selector_ids;
    core().value_branches_.emplace(branch.from, std::move(definition));
  } else {
    definition.end_nodes = std::move(branch.end_nodes);
    definition.selector_ids = std::move(branch.selector_ids);
    core().value_branches_.emplace(std::move(branch.from), std::move(definition));
  }
  return {};
}

template <typename branch_t>
inline auto graph::add_stream_branch_impl(branch_t &&branch) -> wh::core::result<void> {
  auto writable = ensure_writable();
  if (writable.has_error()) {
    return writable;
  }
  if (branch.from.empty()) {
    return fail_fast(wh::core::errc::invalid_argument, "stream-branch source key is empty");
  }
  if (!core().nodes_.contains(branch.from)) {
    return fail_fast(wh::core::errc::not_found,
                     "stream-branch source node not found: " + branch.from);
  }
  if (core().value_branches_.contains(branch.from) ||
      core().stream_branches_.contains(branch.from)) {
    return fail_fast(wh::core::errc::already_exists,
                     "branch source already registered: " + branch.from);
  }
  if (branch.end_nodes.empty()) {
    return fail_fast(wh::core::errc::invalid_argument, "stream-branch end-nodes are empty");
  }

  const auto source_output = authored_output_contract(core().nodes_.at(branch.from));
  if (source_output != node_contract::stream) {
    return fail_fast(wh::core::errc::contract_violation,
                     "stream-branch source must produce stream output: " + branch.from);
  }

  std::unordered_set<std::string_view, detail::string_hash, detail::string_equal>
      unique_end_nodes{};
  unique_end_nodes.reserve(branch.end_nodes.size());
  for (const auto &to : branch.end_nodes) {
    if (!core().nodes_.contains(to)) {
      return fail_fast(wh::core::errc::not_found, "branch end-node not found: " + to);
    }
    if (!unique_end_nodes.insert(to).second) {
      return fail_fast(wh::core::errc::already_exists, "stream-branch end-node duplicated: " + to);
    }
    auto added = add_edge(branch.from, to);
    if (added.has_error()) {
      return added;
    }
  }

  stream_branch_definition definition{};
  if constexpr (std::is_lvalue_reference_v<branch_t>) {
    definition.end_nodes = branch.end_nodes;
    definition.selector_ids = branch.selector_ids;
    core().stream_branches_.emplace(branch.from, std::move(definition));
  } else {
    definition.end_nodes = std::move(branch.end_nodes);
    definition.selector_ids = std::move(branch.selector_ids);
    core().stream_branches_.emplace(std::move(branch.from), std::move(definition));
  }
  return {};
}

inline auto graph::has_edge(const std::string &from, const std::string &to) const -> bool {
  return std::ranges::any_of(
      core().edges_, [&](const graph_edge &edge) { return edge.from == from && edge.to == to; });
}

inline auto graph::init_reserved_nodes() -> void {
  const auto make_boundary_node = [](const std::string_view key, const node_contract contract) {
    switch (contract) {
    case node_contract::value:
      return authored_node{make_passthrough_node<node_contract::value>(std::string{key})};
    case node_contract::stream:
      return authored_node{make_passthrough_node<node_contract::stream>(std::string{key})};
    }
    return authored_node{make_passthrough_node<node_contract::value>(std::string{key})};
  };
  auto start_node = make_boundary_node(graph_start_node_key, core().options_.boundary.input);
  auto end_node = make_boundary_node(graph_end_node_key, core().options_.boundary.output);
  core().nodes_.emplace(std::string{graph_start_node_key}, std::move(start_node));
  core().nodes_.emplace(std::string{graph_end_node_key}, std::move(end_node));
  core().node_insertion_order_.push_back(std::string{graph_start_node_key});
  core().node_insertion_order_.push_back(std::string{graph_end_node_key});
  core().node_id_index_.emplace(std::string{graph_start_node_key}, 0U);
  core().node_id_index_.emplace(std::string{graph_end_node_key}, 1U);
}

inline auto graph::ensure_writable() -> wh::core::result<void> {
  if (core().compiled_) {
    return fail_fast(wh::core::errc::contract_violation, "graph is immutable after compile");
  }
  if (core().first_error_.has_value()) {
    return wh::core::result<void>::failure(*core().first_error_);
  }
  return {};
}

template <typename message_t>
  requires std::constructible_from<std::string, message_t &&>
inline auto graph::fail_fast(const wh::core::error_code code, message_t &&message)
    -> wh::core::result<void> {
  if (!core().first_error_.has_value()) {
    core().first_error_ = code;
  }
  core().diagnostics_.push_back(
      graph_diagnostic{code, std::string{std::forward<message_t>(message)}});
  return wh::core::result<void>::failure(code);
}

} // namespace wh::compose
