// Defines parallel authored builder that lowers one branch group into graph edges.
#pragma once

#include <concepts>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/node/authored.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

/// Fan-out builder that expands one predecessor into multiple authored nodes.
/// This type only lowers graph structure; it is not a runtime executor.
class parallel {
public:
  parallel() = default;

  /// Adds a component node to the parallel group.
  auto add_component(const component_node &node) -> wh::core::result<void> {
    return add_authored(node);
  }

  /// Adds a component node to the parallel group.
  auto add_component(component_node &&node) -> wh::core::result<void> {
    return add_authored(std::move(node));
  }

  /// Adds a lambda node to the parallel group.
  auto add_lambda(const lambda_node &node) -> wh::core::result<void> { return add_authored(node); }

  /// Adds a lambda node to the parallel group.
  auto add_lambda(lambda_node &&node) -> wh::core::result<void> {
    return add_authored(std::move(node));
  }

  /// Adds a subgraph node to the parallel group.
  auto add_subgraph(const subgraph_node &node) -> wh::core::result<void> {
    return add_authored(node);
  }

  /// Adds a subgraph node to the parallel group.
  auto add_subgraph(subgraph_node &&node) -> wh::core::result<void> {
    return add_authored(std::move(node));
  }

  template <typename key_t, typename graph_t, typename options_t = graph_add_node_options>
    requires std::constructible_from<std::string, key_t &&> &&
             std::constructible_from<graph_add_node_options, options_t &&> &&
             graph_viewable<std::remove_cvref_t<graph_t>>
  /// Adds one graph-like object to the parallel group as a subgraph node.
  auto add_subgraph(key_t &&key, graph_t &&subgraph, options_t &&options = {})
      -> wh::core::result<void> {
    return add_authored(make_subgraph_node(std::forward<key_t>(key),
                                           std::forward<graph_t>(subgraph),
                                           std::forward<options_t>(options)));
  }

  /// Adds a tools node to the parallel group.
  auto add_tools(const tools_node &node) -> wh::core::result<void> { return add_authored(node); }

  /// Adds a tools node to the parallel group.
  auto add_tools(tools_node &&node) -> wh::core::result<void> {
    return add_authored(std::move(node));
  }

  /// Adds a passthrough node to the parallel group.
  auto add_passthrough(const passthrough_node &node) -> wh::core::result<void> {
    return add_authored(node);
  }

  /// Adds a passthrough node to the parallel group.
  auto add_passthrough(passthrough_node &&node) -> wh::core::result<void> {
    return add_authored(std::move(node));
  }

  /// Returns the authored nodes that will be lowered from the same predecessor.
  [[nodiscard]] auto nodes() const noexcept -> const std::vector<authored_node> & { return nodes_; }

private:
  template <authored_node_like node_t> auto add_authored(node_t &&node) -> wh::core::result<void> {
    if (node.key().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    nodes_.emplace_back(std::forward<node_t>(node));
    return {};
  }

public:
  /// Lowers the fan-out into `target_graph` and returns the produced tail keys.
  auto apply(graph &target_graph,
             const std::string &predecessor) const & -> wh::core::result<std::vector<std::string>> {
    return apply_impl(target_graph, predecessor, nodes_);
  }

  /// Moves stored nodes into `target_graph` when this builder is an rvalue.
  auto apply(graph &target_graph,
             const std::string &predecessor) && -> wh::core::result<std::vector<std::string>> {
    return apply_impl(target_graph, predecessor, std::move(nodes_));
  }

private:
  template <typename nodes_t>
  auto apply_impl(graph &target_graph, const std::string &predecessor, nodes_t &&source_nodes) const
      -> wh::core::result<std::vector<std::string>> {
    using stored_nodes_t = std::remove_cvref_t<nodes_t>;
    stored_nodes_t stored_nodes{std::forward<nodes_t>(source_nodes)};
    if (stored_nodes.size() < 2U) {
      return wh::core::result<std::vector<std::string>>::failure(wh::core::errc::invalid_argument);
    }

    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        output_keys{};
    output_keys.reserve(stored_nodes.size());
    for (const auto &node : stored_nodes) {
      const auto &options = authored_options(node);
      if (!options.output_key.empty() && !output_keys.insert(options.output_key).second) {
        return wh::core::result<std::vector<std::string>>::failure(wh::core::errc::already_exists);
      }
    }

    std::vector<std::string> next_predecessors{};
    next_predecessors.reserve(stored_nodes.size());
    for (auto &node : stored_nodes) {
      const std::string node_key{authored_key(node)};
      auto added = std::visit(
          [&](auto &value) -> wh::core::result<void> {
            using value_t = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<value_t, component_node>) {
              return target_graph.add_component(std::move(value));
            } else if constexpr (std::same_as<value_t, lambda_node>) {
              return target_graph.add_lambda(std::move(value));
            } else if constexpr (std::same_as<value_t, subgraph_node>) {
              return target_graph.add_subgraph(std::move(value));
            } else if constexpr (std::same_as<value_t, tools_node>) {
              return target_graph.add_tools(std::move(value));
            } else {
              return target_graph.add_passthrough(std::move(value));
            }
          },
          node);
      if (added.has_error()) {
        return wh::core::result<std::vector<std::string>>::failure(added.error());
      }
      auto linked = target_graph.add_edge(predecessor, node_key);
      if (linked.has_error()) {
        return wh::core::result<std::vector<std::string>>::failure(linked.error());
      }
      next_predecessors.push_back(node_key);
    }
    return next_predecessors;
  }
  /// Ordered authored nodes that will share one incoming predecessor edge.
  std::vector<authored_node> nodes_{};
};

} // namespace wh::compose
