// Defines conditional routing builder that lowers authored branch cases to graph edges.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/compose/graph.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

/// One branch case descriptor.
struct branch_case {
  /// Value-condition predicate type.
  using value_predicate = wh::core::callback_function<
      wh::core::result<bool>(const graph_value &, wh::core::run_context &) const>;
  /// Stream-condition predicate type.
  using stream_predicate = wh::core::callback_function<
      wh::core::result<bool>(const graph_stream_reader &,
                             wh::core::run_context &) const>;

  /// Stable case name for diagnostics.
  std::string name{};
  /// Branch destination node key.
  std::string to{};
  /// Value predicate that returns true when this case is selected.
  value_predicate predicate{nullptr};
  /// Optional stream predicate used for stream-condition routing.
  stream_predicate stream_condition{nullptr};
};

/// Conditional routing builder that selects one or more target nodes at runtime.
class branch {
public:
  branch() = default;

  /// Adds one named branch case.
  auto add_case(const branch_case &value) -> wh::core::result<void> {
    if (value.to.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!value.predicate && !value.stream_condition) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    cases_.push_back(value);
    auto &stored = cases_.back();
    if (stored.name.empty()) {
      stored.name = "case_" + std::to_string(cases_.size() - 1U);
    }
    return {};
  }

  /// Adds one named branch case.
  auto add_case(branch_case &&value) -> wh::core::result<void> {
    if (value.to.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!value.predicate && !value.stream_condition) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (value.name.empty()) {
      value.name = "case_" + std::to_string(cases_.size());
    }
    cases_.push_back(std::move(value));
    return {};
  }

  template <typename name_t, typename to_t, typename predicate_t>
    requires std::constructible_from<std::string, name_t &&> &&
             std::constructible_from<std::string, to_t &&>
  /// Adds one named branch case from target + predicate.
  auto add_case(name_t &&name, to_t &&to, predicate_t &&predicate)
      -> wh::core::result<void> {
    return add_case(branch_case{
        .name = std::forward<name_t>(name),
        .to = std::forward<to_t>(to),
        .predicate = [fn = std::forward<predicate_t>(predicate)](
                         const graph_value &value, wh::core::run_context &context)
            -> wh::core::result<bool> { return fn(value, context); },
    });
  }

  template <typename name_t, typename to_t, typename predicate_t>
    requires std::constructible_from<std::string, name_t &&> &&
             std::constructible_from<std::string, to_t &&>
  /// Adds one stream-condition branch case.
  auto add_stream_case(name_t &&name, to_t &&to, predicate_t &&predicate)
      -> wh::core::result<void> {
    return add_case(branch_case{
        .name = std::forward<name_t>(name),
        .to = std::forward<to_t>(to),
        .stream_condition =
            wh::core::callback_function<wh::core::result<bool>(
                const graph_stream_reader &, wh::core::run_context &) const>{
                std::forward<predicate_t>(predicate)},
    });
  }

  /// Returns declared branch destination keys.
  [[nodiscard]] auto end_nodes() const -> std::vector<std::string> {
    std::vector<std::string> keys{};
    keys.reserve(cases_.size());
    std::ranges::copy(cases_ | std::views::transform([](const branch_case &entry)
                                                      -> const std::string & {
                        return entry.to;
                      }),
                      std::back_inserter(keys));
    return keys;
  }

  /// Returns immutable branch cases.
  [[nodiscard]] auto cases() const noexcept -> const std::vector<branch_case> & {
    return cases_;
  }

  /// Applies branch declaration by expanding to graph branch edges.
  auto apply(graph &target_graph, const std::string &from_node_key) const &
      -> wh::core::result<void> {
    return apply_impl(target_graph, from_node_key, cases_);
  }

  /// Move-enabled apply path that avoids extra case copies.
  auto apply(graph &target_graph, const std::string &from_node_key) &&
      -> wh::core::result<void> {
    return apply_impl(target_graph, from_node_key, std::move(cases_));
  }

private:
  template <typename cases_t>
  auto apply_impl(graph &target_graph, const std::string &from_node_key,
                  cases_t &&source_cases) const -> wh::core::result<void> {
    using stored_cases_t = std::remove_cvref_t<cases_t>;
    struct resolved_case {
      std::uint32_t target_id{0U};
      branch_case::value_predicate predicate{nullptr};
      branch_case::stream_predicate stream_condition{nullptr};
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
          .predicate = std::move(entry.predicate),
          .stream_condition = std::move(entry.stream_condition),
      });
      end_nodes.push_back(entry.to);
    }

    graph_branch_selector_ids selector_ids =
        [cases = std::move(resolved_cases)](
            const graph_value &input, wh::core::run_context &context,
            const graph_call_scope &)
            -> wh::core::result<std::vector<std::uint32_t>> {
      std::vector<std::uint32_t> selected{};
      selected.reserve(cases.size());
      for (const auto &entry : cases) {
        bool selected_by_case = false;
        if (entry.predicate) {
          auto matched = entry.predicate(input, context);
          if (matched.has_error()) {
            return wh::core::result<std::vector<std::uint32_t>>::failure(
                matched.error());
          }
          selected_by_case = matched.value();
        } else if (entry.stream_condition) {
          const auto *reader =
              wh::core::any_cast<graph_stream_reader>(&input);
          if (reader != nullptr) {
            auto matched = entry.stream_condition(*reader, context);
            if (matched.has_error()) {
              return wh::core::result<std::vector<std::uint32_t>>::failure(
                  matched.error());
            }
            selected_by_case = matched.value();
          }
        }

        if (!selected_by_case) {
          continue;
        }
        selected.push_back(entry.target_id);
      }
      return selected;
    };

    return target_graph.add_branch(
        graph_branch{
            .from = from_node_key,
            .end_nodes = std::move(end_nodes),
            .selector_ids = std::move(selector_ids),
        });
  }

private:
  /// Ordered branch case list.
  std::vector<branch_case> cases_{};
};

} // namespace wh::compose
