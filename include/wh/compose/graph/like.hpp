// Defines compose graph surface concepts used by nested graph composition.
#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

class graph;

/// Surface that can expose one immutable graph view for nested composition.
template <typename graph_t>
concept graph_viewable =
    std::same_as<std::remove_cvref_t<graph_t>, graph> ||
    requires(const graph_t &value) {
      { value.graph_view() } -> std::same_as<const wh::compose::graph &>;
    };

/// Surface that can relinquish ownership of one materialized graph instance.
template <typename graph_t>
concept graph_owning =
    (std::same_as<std::remove_cvref_t<graph_t>, graph> &&
     std::is_rvalue_reference_v<graph_t &&>) ||
    requires(graph_t &&value) {
      { std::forward<graph_t>(value).release_graph() }
          -> std::same_as<wh::compose::graph>;
    };

namespace detail {

/// Internal nested-graph entry.
/// This keeps subgraph calls on the runtime path instead of going back
/// through `graph.invoke(...)`.
[[nodiscard]] auto start_nested_graph(const graph &graph,
                                      wh::core::run_context &context,
                                      graph_value &input,
                                      const node_runtime &runtime)
    -> graph_sender;

template <typename graph_t>
  requires graph_viewable<graph_t>
[[nodiscard]] inline auto borrow_graph(const graph_t &graph) noexcept
    -> const wh::compose::graph & {
  if constexpr (std::same_as<std::remove_cvref_t<graph_t>, wh::compose::graph>) {
    return graph;
  } else {
    return graph.graph_view();
  }
}

template <typename graph_t>
  requires graph_viewable<graph_t>
[[nodiscard]] inline auto start_nested_graph(const graph_t &graph,
                                             wh::core::run_context &context,
                                             graph_value &input,
                                             const node_runtime &runtime)
    -> graph_sender {
  return start_nested_graph(borrow_graph(graph), context, input, runtime);
}

} // namespace detail

} // namespace wh::compose
