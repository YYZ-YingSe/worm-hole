// Defines value-routing authored branch builder that lowers cases to graph edges.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

/// One value-branch case descriptor.
struct value_branch_case {
  /// Value-condition predicate type.
  using predicate = wh::core::callback_function<wh::core::result<bool>(
      const graph_value &, wh::core::run_context &) const>;

  /// Branch destination node key.
  std::string to{};
  /// Predicate that returns true when this case is selected.
  predicate match{nullptr};
};

/// Conditional value-routing builder that selects one or more target nodes.
class value_branch {
public:
  value_branch() = default;

  /// Adds one value-branch case.
  auto add_case(const value_branch_case &value) -> wh::core::result<void> {
    if (value.to.empty() || !value.match) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    cases_.push_back(value);
    return {};
  }

  /// Adds one value-branch case.
  auto add_case(value_branch_case &&value) -> wh::core::result<void> {
    if (value.to.empty() || !value.match) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    cases_.push_back(std::move(value));
    return {};
  }

  template <typename to_t, typename predicate_t>
    requires std::constructible_from<std::string, to_t &&>
  /// Adds one value-branch case from target + predicate.
  auto add_case(to_t &&to, predicate_t &&predicate) -> wh::core::result<void> {
    return add_case(value_branch_case{
        .to = std::forward<to_t>(to),
        .match = [fn = std::forward<predicate_t>(predicate)](const graph_value &value,
                                                             wh::core::run_context &context)
            -> wh::core::result<bool> { return fn(value, context); },
    });
  }

  /// Returns declared branch destination keys.
  [[nodiscard]] auto end_nodes() const -> std::vector<std::string> {
    std::vector<std::string> keys{};
    keys.reserve(cases_.size());
    std::ranges::copy(
        cases_ | std::views::transform([](const value_branch_case &entry) -> const std::string & {
          return entry.to;
        }),
        std::back_inserter(keys));
    return keys;
  }

  /// Returns immutable branch cases.
  [[nodiscard]] auto cases() const noexcept -> const std::vector<value_branch_case> & {
    return cases_;
  }

  /// Applies value-branch declaration by expanding to graph branch edges.
  auto apply(graph &target_graph,
             const std::string &from_node_key) const & -> wh::core::result<void> {
    return apply_impl(target_graph, from_node_key, cases_);
  }

  /// Move-enabled apply path that avoids extra case copies.
  auto apply(graph &target_graph, const std::string &from_node_key) && -> wh::core::result<void> {
    return apply_impl(target_graph, from_node_key, std::move(cases_));
  }

private:
  template <typename cases_t>
  auto apply_impl(graph &target_graph, const std::string &from_node_key,
                  cases_t &&source_cases) const -> wh::core::result<void> {
    using stored_cases_t = std::remove_cvref_t<cases_t>;
    struct resolved_case {
      std::uint32_t target_id{0U};
      value_branch_case::predicate match{nullptr};
    };

    stored_cases_t stored_cases{std::forward<cases_t>(source_cases)};
    if (stored_cases.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        unique_targets{};
    unique_targets.reserve(stored_cases.size());
    std::vector<std::string> end_nodes{};
    end_nodes.reserve(stored_cases.size());
    std::vector<resolved_case> resolved_cases{};
    resolved_cases.reserve(stored_cases.size());
    for (auto &entry : stored_cases) {
      if (!unique_targets.insert(entry.to).second) {
        return wh::core::result<void>::failure(wh::core::errc::already_exists);
      }
      auto target_id = target_graph.node_id(entry.to);
      if (target_id.has_error()) {
        return wh::core::result<void>::failure(target_id.error());
      }
      resolved_cases.push_back(resolved_case{
          .target_id = target_id.value(),
          .match = std::move(entry.match),
      });
      end_nodes.push_back(entry.to);
    }

    graph_value_branch_selector_ids selector_ids =
        [cases = std::move(resolved_cases)](
            const graph_value &input, wh::core::run_context &context,
            const graph_call_scope &) -> wh::core::result<std::vector<std::uint32_t>> {
      std::vector<std::uint32_t> selected{};
      selected.reserve(cases.size());
      for (const auto &entry : cases) {
        auto matched = entry.match(input, context);
        if (matched.has_error()) {
          return wh::core::result<std::vector<std::uint32_t>>::failure(matched.error());
        }
        if (!matched.value()) {
          continue;
        }
        selected.push_back(entry.target_id);
      }
      return selected;
    };

    return target_graph.add_value_branch(graph_value_branch{
        .from = from_node_key,
        .end_nodes = std::move(end_nodes),
        .selector_ids = std::move(selector_ids),
    });
  }

  /// Ordered value-branch case list.
  std::vector<value_branch_case> cases_{};
};

} // namespace wh::compose
