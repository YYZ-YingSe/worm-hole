// Defines subgraph-node adapters for nesting one compiled graph inside another.
#pragma once

#include <concepts>
#include <string>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/start.hpp"
#include "wh/compose/graph/like.hpp"
#include "wh/compose/node/authored.hpp"

namespace wh::compose {

namespace detail {

template <typename graph_t> [[nodiscard]] inline auto materialize_subgraph(graph_t &&graph) {
  if constexpr (graph_owning<graph_t>) {
    if constexpr (std::same_as<std::remove_cvref_t<graph_t>, wh::compose::graph>) {
      return wh::compose::graph{std::forward<graph_t>(graph)};
    } else {
      return std::forward<graph_t>(graph).release_graph();
    }
  } else {
    using materialized_graph_t = std::remove_cvref_t<decltype(borrow_graph(
        std::declval<const std::remove_cvref_t<graph_t> &>()))>;
    return materialized_graph_t{borrow_graph(graph)};
  }
}

[[nodiscard]] inline auto make_subgraph_compiled_node(std::string key,
                                                      graph_add_node_options options,
                                                      const graph_boundary boundary, auto child)
    -> compiled_node {
  auto subgraph_snapshot = child.snapshot();
  auto subgraph_restore_shape = child.restore_shape();
  auto compiled = make_compiled_async_node(
      node_kind::subgraph, default_exec_origin(node_kind::subgraph), boundary.input,
      boundary.output, std::move(key),
      [child = std::move(child)](graph_value &input, wh::core::run_context &context,
                                 const node_runtime &runtime) mutable -> graph_sender {
        return detail::start_nested_graph(child, context, input, runtime);
      },
      std::move(options));
  compiled.meta.compiled_input_gate = child.boundary_input_gate();
  compiled.meta.compiled_output_gate = child.boundary_output_gate();
  compiled.meta.subgraph_snapshot = std::move(subgraph_snapshot);
  compiled.meta.subgraph_restore_shape = std::move(subgraph_restore_shape);
  return compiled;
}

} // namespace detail

inline auto subgraph_node::compile() const & -> wh::core::result<compiled_node> {
  auto copied = *this;
  return std::move(copied).compile();
}

inline auto subgraph_node::compile() && -> wh::core::result<compiled_node> {
  if (!static_cast<bool>(payload_.lower)) {
    return wh::core::result<compiled_node>::failure(wh::core::errc::not_supported);
  }
  return payload_.lower(std::move(descriptor_.key), std::move(options_));
}

template <typename key_t, typename graph_t, typename options_t = graph_add_node_options>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<graph_add_node_options, options_t &&> &&
           graph_viewable<std::remove_cvref_t<graph_t>>
[[nodiscard]] inline auto make_subgraph_node(key_t &&key, graph_t &&graph, options_t &&options = {})
    -> subgraph_node {
  using stored_graph_t = std::remove_cvref_t<graph_t>;
  const auto &borrowed = detail::borrow_graph(graph);
  const auto boundary = borrowed.boundary();
  auto stored_key = std::string{std::forward<key_t>(key)};
  auto node_options =
      detail::decorate_node_options(std::forward<options_t>(options), "subgraph", "subgraph");
  return subgraph_node{
      node_descriptor{
          .key = std::move(stored_key),
          .kind = node_kind::subgraph,
          .exec_mode = node_exec_mode::async,
          .exec_origin = default_exec_origin(node_kind::subgraph),
          .input_contract = boundary.input,
          .output_contract = boundary.output,
          .input_gate_info = borrowed.boundary_input_gate(),
          .output_gate_info = borrowed.boundary_output_gate(),
      },
      subgraph_payload{.lower = [child = stored_graph_t{std::forward<graph_t>(graph)},
                                 boundary](std::string lowered_key,
                                           graph_add_node_options lowered_options) mutable
                           -> wh::core::result<compiled_node> {
        auto lowered_child = detail::materialize_subgraph(std::move(child));
        if (!lowered_child.compiled()) {
          auto compiled = lowered_child.compile();
          if (compiled.has_error()) {
            return wh::core::result<compiled_node>::failure(compiled.error());
          }
        }
        return detail::make_subgraph_compiled_node(
            std::move(lowered_key), std::move(lowered_options), boundary, std::move(lowered_child));
      }},
      std::move(node_options)};
}

} // namespace wh::compose
