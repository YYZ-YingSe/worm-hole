// Defines field-mapping runtime helpers for workflow data propagation.
#pragma once

#include <concepts>
#include <utility>

#include "wh/compose/field/mapping.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::compose {

namespace detail {

struct pending_field_write {
  const field_path *path{nullptr};
  graph_value value{};
};

[[nodiscard]] inline auto
read_value_by_path(const graph_value_map &source, const field_path &path,
                   const field_missing_policy missing_policy)
    -> wh::core::result<graph_value> {
  const graph_value_map *current = &source;
  for (std::size_t index = 0U; index < path.segments.size(); ++index) {
    const bool last = (index + 1U == path.segments.size());
    const auto iter = current->find(path.segments[index]);
    if (iter == current->end()) {
      if (missing_policy == field_missing_policy::skip) {
        return wh::core::result<graph_value>::failure(
            wh::core::errc::not_found);
      }
      return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
    }

    if (last) {
      return iter->second;
    }

    const auto *nested = wh::core::any_cast<graph_value_map>(&iter->second);
    if (nested == nullptr) {
      return wh::core::result<graph_value>::failure(
          wh::core::errc::type_mismatch);
    }
    current = nested;
  }
  return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
}

template <typename value_t>
  requires std::constructible_from<graph_value, value_t &&>
inline auto write_value_by_path(graph_value_map &target, const field_path &path,
                                value_t &&value) -> wh::core::result<void> {
  graph_value stored_value{std::forward<value_t>(value)};
  graph_value_map *current = &target;
  for (std::size_t index = 0U; index < path.segments.size(); ++index) {
    const bool last = (index + 1U == path.segments.size());
    const auto &segment = path.segments[index];

    if (last) {
      current->insert_or_assign(segment, std::move(stored_value));
      return {};
    }

    auto iter = current->find(segment);
    if (iter == current->end()) {
      iter = current->emplace(segment, graph_value{graph_value_map{}}).first;
    }

    auto *nested = wh::core::any_cast<graph_value_map>(&iter->second);
    if (nested == nullptr) {
      iter->second = graph_value{graph_value_map{}};
      nested = wh::core::any_cast<graph_value_map>(&iter->second);
      if (nested == nullptr) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
    }
    current = nested;
  }
  return {};
}

} // namespace detail

/// Applies compiled mapping rules to `target` in place.
/// The current map is used as the read snapshot, while writes are staged first
/// so one failing rule does not partially mutate the target map.
[[nodiscard]] inline auto apply_field_mappings_in_place(
    graph_value_map &target,
    const std::vector<compiled_field_mapping_rule> &rules,
    wh::core::run_context &context) -> wh::core::result<void> {
  std::vector<detail::pending_field_write> pending_writes{};
  pending_writes.reserve(rules.size());

  for (const auto &rule : rules) {
    wh::core::result<graph_value> extracted{};
    if (rule.static_value.has_value()) {
      extracted = *rule.static_value;
    } else if (rule.extractor) {
      extracted = rule.extractor(target, context);
    } else {
      if (!rule.from_path.has_value()) {
        return wh::core::result<void>::failure(
            wh::core::errc::contract_violation);
      }
      extracted = detail::read_value_by_path(target, *rule.from_path,
                                             rule.missing_policy);
    }

    if (extracted.has_error()) {
      if (extracted.error() == wh::core::errc::not_found &&
          rule.missing_policy == field_missing_policy::skip) {
        continue;
      }
      return wh::core::result<void>::failure(extracted.error());
    }

    pending_writes.push_back(detail::pending_field_write{
        .path = &rule.to_path,
        .value = std::move(extracted).value(),
    });
  }

  for (auto &write : pending_writes) {
    auto written = detail::write_value_by_path(target, *write.path,
                                               std::move(write.value));
    if (written.has_error()) {
      return written;
    }
  }
  return {};
}

/// Applies mapping rules and returns updated target map.
[[nodiscard]] inline auto
apply_field_mappings(const graph_value_map &source, graph_value_map &&target,
                     const std::vector<compiled_field_mapping_rule> &rules,
                     wh::core::run_context &context)
    -> wh::core::result<graph_value_map> {
  for (const auto &rule : rules) {
    wh::core::result<graph_value> extracted{};
    if (rule.static_value.has_value()) {
      extracted = *rule.static_value;
    } else if (rule.extractor) {
      extracted = rule.extractor(source, context);
    } else {
      if (!rule.from_path.has_value()) {
        return wh::core::result<graph_value_map>::failure(
            wh::core::errc::contract_violation);
      }
      extracted = detail::read_value_by_path(source, *rule.from_path,
                                             rule.missing_policy);
    }

    if (extracted.has_error()) {
      if (extracted.error() == wh::core::errc::not_found &&
          rule.missing_policy == field_missing_policy::skip) {
        continue;
      }
      return wh::core::result<graph_value_map>::failure(extracted.error());
    }

    auto written = detail::write_value_by_path(target, rule.to_path,
                                               std::move(extracted).value());
    if (written.has_error()) {
      return wh::core::result<graph_value_map>::failure(written.error());
    }
  }
  return target;
}

/// Applies mapping rules and returns updated target map.
[[nodiscard]] inline auto apply_field_mappings(
    const graph_value_map &source, const graph_value_map &target,
    const std::vector<compiled_field_mapping_rule> &rules,
    wh::core::run_context &context) -> wh::core::result<graph_value_map> {
  graph_value_map copied_target = target;
  return apply_field_mappings(source, std::move(copied_target), rules, context);
}

/// Applies mapping rules and returns updated target map.
[[nodiscard]] inline auto
apply_field_mappings(const graph_value_map &source, graph_value_map &&target,
                     const std::vector<field_mapping_rule> &rules,
                     wh::core::run_context &context)
    -> wh::core::result<graph_value_map> {
  std::vector<compiled_field_mapping_rule> compiled_rules{};
  compiled_rules.reserve(rules.size());
  for (const auto &rule : rules) {
    auto compiled_rule = compile_field_mapping_rule(rule);
    if (compiled_rule.has_error()) {
      return wh::core::result<graph_value_map>::failure(compiled_rule.error());
    }
    compiled_rules.push_back(std::move(compiled_rule).value());
  }

  return apply_field_mappings(source, std::move(target), compiled_rules,
                              context);
}

/// Applies mapping rules and returns updated target map.
[[nodiscard]] inline auto apply_field_mappings(
    const graph_value_map &source, const graph_value_map &target,
    const std::vector<field_mapping_rule> &rules,
    wh::core::run_context &context) -> wh::core::result<graph_value_map> {
  graph_value_map copied_target = target;
  return apply_field_mappings(source, std::move(copied_target), rules, context);
}

} // namespace wh::compose
