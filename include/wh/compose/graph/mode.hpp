// Defines fixed-runtime-mode graph wrappers for DAG and Pregel facades.
#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/compose/graph/compile_options.hpp"
#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

template <graph_runtime_mode Mode> class mode_graph {
private:
  [[nodiscard]] static constexpr auto default_name() noexcept -> std::string_view {
    if constexpr (Mode == graph_runtime_mode::pregel) {
      return "pregel";
    } else {
      return "dag";
    }
  }

  template <typename options_t>
  [[nodiscard]] static auto normalize_options(options_t &&options) -> graph_compile_options {
    auto compiled = graph_compile_options{std::forward<options_t>(options)};
    compiled.mode = Mode;
    return compiled;
  }

public:
  mode_graph()
      : graph_([] {
          graph_compile_options options{};
          options.name = std::string{default_name()};
          options.mode = Mode;
          return options;
        }()) {}

  explicit mode_graph(const graph_compile_options &options)
      : graph_(normalize_options<const graph_compile_options &>(options)) {}

  explicit mode_graph(graph_compile_options &&options)
      : graph_(normalize_options<graph_compile_options>(std::move(options))) {}

  /// Returns immutable compile options with the fixed runtime mode.
  [[nodiscard]] auto options() const noexcept -> const graph_compile_options & {
    return graph_.options();
  }

  /// Returns compile options snapshot used by diff and restore validation.
  [[nodiscard]] auto compile_options_snapshot() const -> graph_compile_options {
    return graph_.compile_options_snapshot();
  }

  /// Returns true after compile succeeds.
  [[nodiscard]] auto compiled() const noexcept -> bool { return graph_.compiled(); }

  /// Returns diagnostics collected during build/compile/invoke.
  [[nodiscard]] auto diagnostics() const noexcept -> const std::vector<graph_diagnostic> & {
    return graph_.diagnostics();
  }

  /// Returns compile order captured by the latest successful compile.
  [[nodiscard]] auto compile_order() const noexcept -> const std::vector<std::string> & {
    return graph_.compile_order();
  }

  /// Returns the underlying graph view for generic compose integration.
  [[nodiscard]] auto graph_view() const noexcept -> const graph & { return graph_; }

  /// Returns a mutable graph view for internal generic integrations.
  [[nodiscard]] auto graph_view() noexcept -> graph & { return graph_; }

  /// Releases the owned graph while keeping the fixed mode guarantee.
  [[nodiscard]] auto release_graph() && noexcept -> graph { return std::move(graph_); }

  /// Registers a component node.
  auto add_component(const component_node &node) -> wh::core::result<void> {
    return graph_.add_component(node);
  }

  /// Registers a component node.
  auto add_component(component_node &&node) -> wh::core::result<void> {
    return graph_.add_component(std::move(node));
  }

  /// Registers a lambda node.
  auto add_lambda(const lambda_node &node) -> wh::core::result<void> {
    return graph_.add_lambda(node);
  }

  /// Registers a lambda node.
  auto add_lambda(lambda_node &&node) -> wh::core::result<void> {
    return graph_.add_lambda(std::move(node));
  }

  /// Registers a subgraph node.
  auto add_subgraph(const subgraph_node &node) -> wh::core::result<void> {
    return graph_.add_subgraph(node);
  }

  /// Registers a subgraph node.
  auto add_subgraph(subgraph_node &&node) -> wh::core::result<void> {
    return graph_.add_subgraph(std::move(node));
  }

  /// Registers a tools node.
  auto add_tools(const tools_node &node) -> wh::core::result<void> {
    return graph_.add_tools(node);
  }

  /// Registers a tools node.
  auto add_tools(tools_node &&node) -> wh::core::result<void> {
    return graph_.add_tools(std::move(node));
  }

  /// Registers a passthrough node.
  auto add_passthrough(const passthrough_node &node) -> wh::core::result<void> {
    return graph_.add_passthrough(node);
  }

  /// Registers a passthrough node.
  auto add_passthrough(passthrough_node &&node) -> wh::core::result<void> {
    return graph_.add_passthrough(std::move(node));
  }

  template <node_contract From = node_contract::value, node_contract To = node_contract::value,
            node_exec_mode Exec = node_exec_mode::sync, typename key_t, typename lambda_t,
            typename options_t = graph_add_node_options>
  /// Registers a lambda node.
  auto add_lambda(key_t &&key, lambda_t &&lambda, options_t &&options = {})
      -> wh::core::result<void> {
    return graph_.add_lambda<From, To, Exec>(
        std::forward<key_t>(key), std::forward<lambda_t>(lambda), std::forward<options_t>(options));
  }

  template <component_kind Kind, node_contract From, node_contract To,
            node_exec_mode Exec = node_exec_mode::sync, typename key_t, typename component_t,
            typename options_t = graph_add_node_options>
    requires(Kind != component_kind::custom)
  /// Registers a component node.
  auto add_component(key_t &&key, component_t &&component, options_t &&options = {})
      -> wh::core::result<void> {
    return graph_.add_component<Kind, From, To, Exec>(std::forward<key_t>(key),
                                                      std::forward<component_t>(component),
                                                      std::forward<options_t>(options));
  }

  template <typename key_t, typename graph_t, typename options_t = graph_add_node_options>
  /// Registers a subgraph node.
  auto add_subgraph(key_t &&key, graph_t &&subgraph, options_t &&options = {})
      -> wh::core::result<void> {
    return graph_.add_subgraph(std::forward<key_t>(key), std::forward<graph_t>(subgraph),
                               std::forward<options_t>(options));
  }

  template <node_contract From = node_contract::value, node_contract To = node_contract::value,
            node_exec_mode Exec = node_exec_mode::sync, typename key_t, typename registry_t,
            typename options_t = graph_add_node_options, typename tool_options_t = tools_options>
  /// Registers a tools node.
  auto add_tools(key_t &&key, registry_t &&registry, options_t &&options = {},
                 tool_options_t &&tool_options = {}) -> wh::core::result<void> {
    return graph_.add_tools<From, To, Exec>(
        std::forward<key_t>(key), std::forward<registry_t>(registry),
        std::forward<options_t>(options), std::forward<tool_options_t>(tool_options));
  }

  template <node_contract Contract = node_contract::value, typename key_t>
  /// Registers a passthrough node.
  auto add_passthrough(key_t &&key) -> wh::core::result<void> {
    return graph_.add_passthrough<Contract>(std::forward<key_t>(key));
  }

  /// Registers one edge between two existing nodes.
  auto add_edge(const graph_edge &edge) -> wh::core::result<void> { return graph_.add_edge(edge); }

  /// Registers one edge between two existing nodes.
  auto add_edge(graph_edge &&edge) -> wh::core::result<void> {
    return graph_.add_edge(std::move(edge));
  }

  template <typename from_t, typename to_t>
    requires std::constructible_from<std::string, from_t &&> &&
             std::constructible_from<std::string, to_t &&>
  /// Registers one edge from source key to target key with edge options.
  auto add_edge(from_t &&from, to_t &&to, edge_options options = {}) -> wh::core::result<void> {
    return graph_.add_edge(std::forward<from_t>(from), std::forward<to_t>(to), std::move(options));
  }

  template <typename to_t>
    requires std::constructible_from<std::string, to_t &&>
  /// Registers one edge from the reserved graph entry node.
  auto add_entry_edge(to_t &&to, edge_options options = {}) -> wh::core::result<void> {
    return graph_.add_entry_edge(std::forward<to_t>(to), std::move(options));
  }

  template <typename from_t>
    requires std::constructible_from<std::string, from_t &&>
  /// Registers one edge into the reserved graph exit node.
  auto add_exit_edge(from_t &&from, edge_options options = {}) -> wh::core::result<void> {
    return graph_.add_exit_edge(std::forward<from_t>(from), std::move(options));
  }

  /// Registers one value-branch declaration by expanding to multiple edges.
  auto add_value_branch(const graph_value_branch &branch) -> wh::core::result<void> {
    return graph_.add_value_branch(branch);
  }

  /// Registers one value-branch declaration by expanding to multiple edges.
  auto add_value_branch(graph_value_branch &&branch) -> wh::core::result<void> {
    return graph_.add_value_branch(std::move(branch));
  }

  /// Registers one stream-branch declaration by expanding to multiple edges.
  auto add_stream_branch(const graph_stream_branch &branch) -> wh::core::result<void> {
    return graph_.add_stream_branch(branch);
  }

  /// Registers one stream-branch declaration by expanding to multiple edges.
  auto add_stream_branch(graph_stream_branch &&branch) -> wh::core::result<void> {
    return graph_.add_stream_branch(std::move(branch));
  }

  /// Runs compile validation and freezes graph structure.
  auto compile() -> wh::core::result<void> { return graph_.compile(); }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
  /// Invokes the fixed-mode graph with typed controls/services.
  [[nodiscard]] auto invoke(wh::core::run_context &context, request_t &&request) const -> auto {
    return graph_.invoke(context, std::forward<request_t>(request));
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
  /// Invokes the fixed-mode graph with explicit control/work schedulers.
  [[nodiscard]] auto invoke(wh::core::run_context &context, request_t &&request,
                            graph_invoke_schedulers schedulers) const -> auto {
    return graph_.invoke(context, std::forward<request_t>(request), std::move(schedulers));
  }

private:
  graph graph_{};
};

} // namespace wh::compose
