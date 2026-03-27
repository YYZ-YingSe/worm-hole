// Defines standalone fan-out helper utilities used by compose parallel flows.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/compose/types.hpp"
#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

/// Reduce policy used by dynamic fan-out execution.
enum class parallel_reduce_policy : std::uint8_t {
  /// Any child failure fails the whole fan-out.
  all_success = 0U,
  /// Keep successful children and surface per-child failures in result payload.
  partial_success,
  /// Stop at first child failure and fail immediately.
  first_error,
};

/// Partition strategy used by fan-out target grouping.
enum class parallel_partition_mode : std::uint8_t {
  /// Partition by stable target key.
  by_key = 0U,
  /// Partition by fixed bucket capacity.
  by_capacity,
};

/// One dynamic fan-out target.
struct parallel_target {
  /// Stable target key for routing/diagnostics.
  std::string key{};
  /// Per-target input payload.
  graph_value input{};
};

/// One fan-out child execution outcome.
struct parallel_child_outcome {
  /// Target key that produced this outcome.
  std::string key{};
  /// True when child execution succeeded.
  bool success{false};
  /// Child output when `success == true`.
  graph_value output{};
  /// Child error when `success == false`.
  wh::core::error_code error{wh::core::errc::ok};
};

/// Aggregated fan-out reduce result.
struct parallel_reduce_result {
  /// Ordered child outcomes in deterministic target order.
  std::vector<parallel_child_outcome> outcomes{};
};

/// Fan-out partition options.
struct parallel_partition_options {
  /// Partition mode.
  parallel_partition_mode mode{parallel_partition_mode::by_capacity};
  /// Partition capacity when `mode == by_capacity`.
  std::size_t capacity{1U};
};

/// Dynamic route callback used to produce fan-out targets from runtime input.
using parallel_route_callback = wh::core::callback_function<
    wh::core::result<std::vector<parallel_target>>(const graph_value &,
                                                   wh::core::run_context &) const>;

/// Dynamic map callback executed for each fan-out target.
using parallel_map_callback = wh::core::callback_function<
    wh::core::result<graph_value>(const parallel_target &,
                                  wh::core::run_context &) const>;

/// Partitions fan-out targets into deterministic groups.
[[nodiscard]] inline auto partition_parallel_targets(
    const std::vector<parallel_target> &targets,
    parallel_partition_options options = {})
    -> wh::core::result<std::vector<std::vector<parallel_target>>> {
  if (targets.empty()) {
    return std::vector<std::vector<parallel_target>>{};
  }

  std::vector<std::vector<parallel_target>> groups{};
  if (options.mode == parallel_partition_mode::by_key) {
    std::unordered_map<std::string, std::size_t,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        key_to_index{};
    key_to_index.reserve(targets.size());
    for (const auto &target : targets) {
      const auto found = key_to_index.find(target.key);
      if (found == key_to_index.end()) {
        key_to_index.insert_or_assign(target.key, groups.size());
        groups.push_back({});
        groups.back().push_back(target);
        continue;
      }
      groups[found->second].push_back(target);
    }
    return groups;
  }

  if (options.capacity == 0U) {
    return wh::core::result<std::vector<std::vector<parallel_target>>>::failure(
        wh::core::errc::invalid_argument);
  }
  groups.reserve((targets.size() + options.capacity - 1U) / options.capacity);
  for (std::size_t index = 0U; index < targets.size(); index += options.capacity) {
    const auto stop = std::min(index + options.capacity, targets.size());
    groups.emplace_back(targets.begin() + static_cast<std::ptrdiff_t>(index),
                        targets.begin() + static_cast<std::ptrdiff_t>(stop));
  }
  return groups;
}

/// Move-enabled partition helper that avoids extra target copies.
[[nodiscard]] inline auto partition_parallel_targets(
    std::vector<parallel_target> &&targets, parallel_partition_options options = {})
    -> wh::core::result<std::vector<std::vector<parallel_target>>> {
  if (targets.empty()) {
    return std::vector<std::vector<parallel_target>>{};
  }

  std::vector<std::vector<parallel_target>> groups{};
  if (options.mode == parallel_partition_mode::by_key) {
    std::unordered_map<std::string, std::size_t,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        key_to_index{};
    key_to_index.reserve(targets.size());
    for (auto &target : targets) {
      const auto found = key_to_index.find(target.key);
      if (found == key_to_index.end()) {
        key_to_index.insert_or_assign(target.key, groups.size());
        groups.push_back({});
        groups.back().push_back(std::move(target));
        continue;
      }
      groups[found->second].push_back(std::move(target));
    }
    return groups;
  }

  if (options.capacity == 0U) {
    return wh::core::result<std::vector<std::vector<parallel_target>>>::failure(
        wh::core::errc::invalid_argument);
  }
  groups.reserve((targets.size() + options.capacity - 1U) / options.capacity);
  std::vector<parallel_target> bucket{};
  bucket.reserve(options.capacity);
  for (auto &target : targets) {
    bucket.push_back(std::move(target));
    if (bucket.size() < options.capacity) {
      continue;
    }
    groups.push_back(std::move(bucket));
    bucket = {};
    bucket.reserve(options.capacity);
  }
  if (!bucket.empty()) {
    groups.push_back(std::move(bucket));
  }
  return groups;
}

template <typename route_t, typename map_t>
  requires std::constructible_from<parallel_route_callback, route_t &&> &&
           std::constructible_from<parallel_map_callback, map_t &&>
/// Executes one local fan-out/map/reduce helper flow in deterministic target order.
[[nodiscard]] inline auto execute_parallel_fanout(
    const graph_value &input, wh::core::run_context &context, route_t &&route,
    map_t &&map,
    const parallel_reduce_policy reduce_policy = parallel_reduce_policy::all_success)
    -> wh::core::result<parallel_reduce_result> {
  parallel_route_callback route_callback{std::forward<route_t>(route)};
  parallel_map_callback map_callback{std::forward<map_t>(map)};
  if (!route_callback || !map_callback) {
    return wh::core::result<parallel_reduce_result>::failure(
        wh::core::errc::invalid_argument);
  }

  auto routed = route_callback(input, context);
  if (routed.has_error()) {
    return wh::core::result<parallel_reduce_result>::failure(routed.error());
  }

  parallel_reduce_result reduced{};
  reduced.outcomes.reserve(routed.value().size());
  for (const auto &target : routed.value()) {
    auto mapped = map_callback(target, context);
    if (mapped.has_error()) {
      if (reduce_policy == parallel_reduce_policy::first_error) {
        return wh::core::result<parallel_reduce_result>::failure(mapped.error());
      }
      reduced.outcomes.push_back(parallel_child_outcome{
          .key = target.key,
          .success = false,
          .output = {},
          .error = mapped.error(),
      });
      continue;
    }
    reduced.outcomes.push_back(parallel_child_outcome{
        .key = target.key,
        .success = true,
        .output = std::move(mapped).value(),
        .error = wh::core::errc::ok,
    });
  }

  if (reduce_policy == parallel_reduce_policy::all_success) {
    const auto failed = std::ranges::find_if(
        reduced.outcomes, [](const parallel_child_outcome &outcome) -> bool {
          return !outcome.success;
        });
    if (failed != reduced.outcomes.end()) {
      return wh::core::result<parallel_reduce_result>::failure(failed->error);
    }
  }
  return reduced;
}

} // namespace wh::compose
