// Defines stream-routing authored branch builder that lowers one selector to graph edges.
#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/compose/graph.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

/// Stream-branch selector that returns selected destination node keys.
using stream_branch_key_sender =
    wh::core::detail::result_sender<wh::core::result<std::vector<std::string>>>;
using stream_branch_selector =
    wh::core::callback_function<stream_branch_key_sender(graph_stream_reader,
                                                         wh::core::run_context &) const>;

/// Conditional stream-routing builder that selects one or more target nodes.
class stream_branch {
public:
  stream_branch() = default;

  template <typename to_t>
    requires std::constructible_from<std::string, to_t &&>
  /// Adds one allowed destination node key.
  auto add_target(to_t &&to) -> wh::core::result<void> {
    const std::string target{std::forward<to_t>(to)};
    if (target.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    targets_.push_back(target);
    return {};
  }

  template <typename selector_t>
    requires std::constructible_from<stream_branch_selector, selector_t &&>
  /// Replaces the stream selector used to route one selection reader.
  auto set_selector(selector_t &&selector) -> wh::core::result<void> {
    selector_ = stream_branch_selector{std::forward<selector_t>(selector)};
    if (!selector_) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    return {};
  }

  /// Returns declared branch destination keys.
  [[nodiscard]] auto end_nodes() const -> std::vector<std::string> { return targets_; }

  /// Returns immutable branch destination keys.
  [[nodiscard]] auto targets() const noexcept -> const std::vector<std::string> & {
    return targets_;
  }

  /// Returns the optional selector used by this branch.
  [[nodiscard]] auto selector() const noexcept -> const stream_branch_selector & {
    return selector_;
  }

  /// Applies stream-branch declaration by expanding to graph branch edges.
  auto apply(graph &target_graph,
             const std::string &from_node_key) const & -> wh::core::result<void> {
    return apply_impl(target_graph, from_node_key, targets_, selector_);
  }

  /// Move-enabled apply path that avoids extra target copies.
  auto apply(graph &target_graph, const std::string &from_node_key) && -> wh::core::result<void> {
    return apply_impl(target_graph, from_node_key, std::move(targets_), std::move(selector_));
  }

private:
  template <typename targets_t, typename selector_t>
  auto apply_impl(graph &target_graph, const std::string &from_node_key, targets_t &&source_targets,
                  selector_t &&selector) const -> wh::core::result<void> {
    using stored_targets_t = std::remove_cvref_t<targets_t>;
    stored_targets_t stored_targets{std::forward<targets_t>(source_targets)};
    if (stored_targets.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        unique_targets{};
    unique_targets.reserve(stored_targets.size());
    std::unordered_map<std::string, std::uint32_t, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        target_ids{};
    target_ids.reserve(stored_targets.size());
    for (const auto &target : stored_targets) {
      if (!unique_targets.insert(target).second) {
        return wh::core::result<void>::failure(wh::core::errc::already_exists);
      }
      auto target_id = target_graph.node_id(target);
      if (target_id.has_error()) {
        return wh::core::result<void>::failure(target_id.error());
      }
      target_ids.emplace(target, target_id.value());
    }

    graph_stream_branch_selector_ids selector_ids{nullptr};
    stream_branch_selector stored_selector{std::forward<selector_t>(selector)};
    if (stored_selector) {
      selector_ids =
          [selector_fn = std::move(stored_selector),
           target_ids = std::move(target_ids)](graph_stream_reader input,
                                               wh::core::run_context &context,
                                               const graph_call_scope &) -> graph_branch_ids_sender {
        return graph_branch_ids_sender{
            wh::core::detail::map_result_sender<wh::core::result<std::vector<std::uint32_t>>>(
                selector_fn(std::move(input), context),
                [target_ids](std::vector<std::string> selected_keys) mutable
                    -> wh::core::result<std::vector<std::uint32_t>> {
                  std::vector<std::uint32_t> selected{};
                  selected.reserve(selected_keys.size());
                  for (const auto &key : selected_keys) {
                    const auto iter = target_ids.find(key);
                    if (iter == target_ids.end()) {
                      return wh::core::result<std::vector<std::uint32_t>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    selected.push_back(iter->second);
                  }
                  return selected;
                })};
      };
    }

    return target_graph.add_stream_branch(graph_stream_branch{
        .from = from_node_key,
        .end_nodes = std::move(stored_targets),
        .selector_ids = std::move(selector_ids),
    });
  }

  /// Ordered branch destination keys.
  std::vector<std::string> targets_{};
  /// Optional selector that routes one copied selection reader.
  stream_branch_selector selector_{nullptr};
};

} // namespace wh::compose
