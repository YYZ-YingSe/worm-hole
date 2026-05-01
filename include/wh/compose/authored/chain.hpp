// Defines linear authored compose builder that lowers append operations into graph IR.
#pragma once

#include <concepts>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/compose/authored/parallel.hpp"
#include "wh/compose/authored/stream_branch.hpp"
#include "wh/compose/authored/value_branch.hpp"
#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/graph/detail/invoke.hpp"
#include "wh/compose/graph/detail/inline_impl.hpp"
#include "wh/compose/graph/invoke_types.hpp"
#include "wh/compose/node/authored.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

/// Linear graph builder that connects each appended step after the current tail set.
/// It lowers to `graph` and executes through the regular graph runtime.
class chain {
public:
  chain() = default;
  explicit chain(const graph_compile_options &options) : graph_(options) {}
  explicit chain(graph_compile_options &&options) : graph_(std::move(options)) {}

  /// Appends a component node after the current tail set.
  auto append(const component_node &node) -> wh::core::result<void> {
    return append_authored(node);
  }

  /// Appends a component node after the current tail set.
  auto append(component_node &&node) -> wh::core::result<void> {
    return append_authored(std::move(node));
  }

  /// Appends a lambda node after the current tail set.
  auto append(const lambda_node &node) -> wh::core::result<void> { return append_authored(node); }

  /// Appends a lambda node after the current tail set.
  auto append(lambda_node &&node) -> wh::core::result<void> {
    return append_authored(std::move(node));
  }

  /// Appends a subgraph node after the current tail set.
  auto append(const subgraph_node &node) -> wh::core::result<void> { return append_authored(node); }

  /// Appends a subgraph node after the current tail set.
  auto append(subgraph_node &&node) -> wh::core::result<void> {
    return append_authored(std::move(node));
  }

  template <typename key_t, typename graph_t, typename options_t = graph_add_node_options>
    requires std::constructible_from<std::string, key_t &&> &&
             std::constructible_from<graph_add_node_options, options_t &&> &&
             graph_viewable<std::remove_cvref_t<graph_t>>
  /// Appends a graph-like object as one subgraph node after the current tail set.
  auto append_subgraph(key_t &&key, graph_t &&subgraph, options_t &&options = {})
      -> wh::core::result<void> {
    return append(make_subgraph_node(std::forward<key_t>(key), std::forward<graph_t>(subgraph),
                                     std::forward<options_t>(options)));
  }

  /// Appends a tools node after the current tail set.
  auto append(const tools_node &node) -> wh::core::result<void> { return append_authored(node); }

  /// Appends a tools node after the current tail set.
  auto append(tools_node &&node) -> wh::core::result<void> {
    return append_authored(std::move(node));
  }

  /// Appends a passthrough node after the current tail set.
  auto append(const passthrough_node &node) -> wh::core::result<void> {
    return append_authored(node);
  }

  /// Appends a passthrough node after the current tail set.
  auto append(passthrough_node &&node) -> wh::core::result<void> {
    return append_authored(std::move(node));
  }

private:
  template <authored_node_like node_t>
  auto append_authored(node_t &&node) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }

    const std::string tail_key{node.key()};
    auto added = [&]() -> wh::core::result<void> {
      using stored_t = std::remove_cvref_t<node_t>;
      if constexpr (std::same_as<stored_t, component_node>) {
        return graph_.add_component(std::forward<node_t>(node));
      } else if constexpr (std::same_as<stored_t, lambda_node>) {
        return graph_.add_lambda(std::forward<node_t>(node));
      } else if constexpr (std::same_as<stored_t, subgraph_node>) {
        return graph_.add_subgraph(std::forward<node_t>(node));
      } else if constexpr (std::same_as<stored_t, tools_node>) {
        return graph_.add_tools(std::forward<node_t>(node));
      } else {
        return graph_.add_passthrough(std::forward<node_t>(node));
      }
    }();
    if (added.has_error()) {
      return fail_fast(added.error());
    }
    if (tails_.empty()) {
      auto linked = graph_.add_entry_edge(tail_key);
      if (linked.has_error()) {
        return fail_fast(linked.error());
      }
    } else {
      for (const auto &tail : tails_) {
        auto linked = graph_.add_edge(tail, tail_key);
        if (linked.has_error()) {
          return fail_fast(linked.error());
        }
      }
    }

    tails_.clear();
    tails_.push_back(tail_key);
    return {};
  }

public:
  /// Appends one branch from current single-tail predecessor.
  auto append(const value_branch &value) -> wh::core::result<void> { return append_branch(value); }

  /// Appends one branch from current single-tail predecessor.
  auto append(value_branch &&value) -> wh::core::result<void> {
    return append_branch(std::move(value));
  }

  /// Appends one stream-branch from current single-tail predecessor.
  auto append(const stream_branch &value) -> wh::core::result<void> { return append_branch(value); }

  /// Appends one stream-branch from current single-tail predecessor.
  auto append(stream_branch &&value) -> wh::core::result<void> {
    return append_branch(std::move(value));
  }

  /// Appends one parallel fan-out from current single-tail predecessor.
  auto append(const parallel &value) -> wh::core::result<void> { return append_parallel(value); }

  /// Appends one parallel fan-out from current single-tail predecessor.
  auto append(parallel &&value) -> wh::core::result<void> {
    return append_parallel(std::move(value));
  }

  /// Appends one branch from current single-tail predecessor.
  auto append_branch(const value_branch &value) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    if (tails_.size() != 1U) {
      return fail_fast(wh::core::errc::contract_violation);
    }

    auto applied = value.apply(graph_, tails_.front());
    if (applied.has_error()) {
      return fail_fast(applied.error());
    }
    tails_ = value.end_nodes();
    return {};
  }

  /// Appends one branch from current single-tail predecessor.
  auto append_branch(value_branch &&value) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    if (tails_.size() != 1U) {
      return fail_fast(wh::core::errc::contract_violation);
    }

    auto next_tails = value.end_nodes();
    auto applied = std::move(value).apply(graph_, tails_.front());
    if (applied.has_error()) {
      return fail_fast(applied.error());
    }
    tails_ = std::move(next_tails);
    return {};
  }

  /// Appends one stream-branch from current single-tail predecessor.
  auto append_branch(const stream_branch &value) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    if (tails_.size() != 1U) {
      return fail_fast(wh::core::errc::contract_violation);
    }

    auto applied = value.apply(graph_, tails_.front());
    if (applied.has_error()) {
      return fail_fast(applied.error());
    }
    tails_ = value.end_nodes();
    return {};
  }

  /// Appends one stream-branch from current single-tail predecessor.
  auto append_branch(stream_branch &&value) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    if (tails_.size() != 1U) {
      return fail_fast(wh::core::errc::contract_violation);
    }

    auto next_tails = value.end_nodes();
    auto applied = std::move(value).apply(graph_, tails_.front());
    if (applied.has_error()) {
      return fail_fast(applied.error());
    }
    tails_ = std::move(next_tails);
    return {};
  }

  /// Appends one parallel fan-out from current single-tail predecessor.
  auto append_parallel(const parallel &value) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    if (tails_.size() != 1U) {
      return fail_fast(wh::core::errc::contract_violation);
    }

    auto applied = value.apply(graph_, tails_.front());
    if (applied.has_error()) {
      return fail_fast(applied.error());
    }
    tails_ = std::move(applied).value();
    return {};
  }

  /// Appends one parallel fan-out from current single-tail predecessor.
  auto append_parallel(parallel &&value) -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }
    if (tails_.size() != 1U) {
      return fail_fast(wh::core::errc::contract_violation);
    }

    auto applied = std::move(value).apply(graph_, tails_.front());
    if (applied.has_error()) {
      return fail_fast(applied.error());
    }
    tails_ = std::move(applied).value();
    return {};
  }

  /// Compiles underlying graph and freezes chain mutations.
  auto compile() -> wh::core::result<void> {
    auto writable = ensure_writable();
    if (writable.has_error()) {
      return writable;
    }

    if (tails_.empty()) {
      return fail_fast(wh::core::errc::contract_violation);
    }
    for (const auto &tail : tails_) {
      auto linked = graph_.add_exit_edge(tail);
      if (linked.has_error() && linked.error() != wh::core::errc::already_exists) {
        return fail_fast(linked.error());
      }
    }

    auto compiled = graph_.compile();
    if (compiled.has_error()) {
      return fail_fast(compiled.error());
    }
    compiled_ = true;
    return {};
  }

  template <typename input_t>
    requires std::same_as<std::remove_cvref_t<input_t>, graph_value>
  /// Executes the lowered graph through the typed graph invoke request.
  [[nodiscard]] auto invoke(wh::core::run_context &context, input_t &&input) const {
    wh::compose::graph_invoke_request request{};
    request.input = wh::compose::graph_input::value(std::forward<input_t>(input));
    return wh::core::detail::map_result_sender<wh::core::result<graph_value>>(
        graph_.invoke(context, std::move(request)),
        [](graph_invoke_result invoke_result) { return std::move(invoke_result.output_status); });
  }

  /// Returns the underlying lowered graph for inspection or nested composition.
  [[nodiscard]] auto graph_view() const noexcept -> const graph & { return graph_; }

  /// Releases the lowered graph for nesting or ownership transfer.
  [[nodiscard]] auto release_graph() && noexcept -> graph { return std::move(graph_); }

private:
  auto ensure_writable() -> wh::core::result<void> {
    if (compiled_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (first_error_.has_value()) {
      return wh::core::result<void>::failure(*first_error_);
    }
    return {};
  }

  auto fail_fast(const wh::core::error_code code) -> wh::core::result<void> {
    if (!first_error_.has_value()) {
      first_error_ = code;
    }
    return wh::core::result<void>::failure(code);
  }

  /// Underlying graph IR.
  graph graph_{};
  /// Current tail nodes used by append operations.
  std::vector<std::string> tails_{};
  /// Chain mutation freeze flag after compile.
  bool compiled_{false};
  /// First error retained for stable fail-fast behavior.
  std::optional<wh::core::error_code> first_error_{};
};

} // namespace wh::compose
