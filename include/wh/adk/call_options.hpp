// Defines ADK call-option overlay and scope-materialization helpers without
// copying compose or component runtime option stacks.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::adk {

/// Transparent hash alias used by ADK option dictionaries.
using option_string_hash = wh::core::transparent_string_hash;

/// Transparent equality alias used by ADK option dictionaries.
using option_string_equal = wh::core::transparent_string_equal;

/// Type-erased option bag shared by ADK call overlay layers.
using option_bag = std::unordered_map<std::string, wh::core::any,
                                      option_string_hash, option_string_equal>;

/// One named option bag scoped to a single agent or tool target.
struct named_option_bag {
  /// Stable scope target for this bag.
  std::string name{};
  /// Options visible only to `name`.
  option_bag values{};
};

/// History-trim knobs applied at transfer or bridge boundaries.
struct transfer_trim_options {
  /// When set, assistant transfer messages are dropped on the targeted path.
  std::optional<bool> trim_assistant_transfer_message{};
  /// When set, transfer tool-call/request pairs are dropped on the targeted
  /// path.
  std::optional<bool> trim_tool_transfer_pair{};
};

/// Resolved history-trim policy after layered option overlay.
struct resolved_transfer_trim_options {
  /// True means assistant transfer messages must be trimmed.
  bool trim_assistant_transfer_message{false};
  /// True means transfer tool-call/request pairs must be trimmed.
  bool trim_tool_transfer_pair{false};
};

/// Shared budget and circuit-breaker knobs for ADK-driven calls.
struct call_budget_options {
  /// Optional maximum concurrent fanout or tool work for one call tree.
  std::optional<std::size_t> max_concurrency{};
  /// Optional maximum iteration or step budget for one authored loop.
  std::optional<std::size_t> max_iterations{};
  /// Optional token budget used by governance or reduction layers.
  std::optional<std::size_t> token_budget{};
  /// Optional breaker threshold for repeated failures.
  std::optional<std::size_t> circuit_breaker_threshold{};
  /// Optional timeout budget applied at the call surface.
  std::optional<std::chrono::milliseconds> timeout{};
  /// Optional fail-fast flag for subtree short-circuit.
  std::optional<bool> fail_fast{};
};

/// One fully layered ADK call-options object.
struct call_options {
  /// Global options visible to every agent or tool unless shadowed.
  option_bag global{};
  /// Checkpoint-visible fields bridged to restore paths.
  option_bag checkpoint_fields{};
  /// Agent-targeted option scopes.
  std::vector<named_option_bag> agent_scopes{};
  /// Tool-targeted option scopes.
  std::vector<named_option_bag> tool_scopes{};
  /// Implementation-specific option namespaces keyed by backend name.
  std::unordered_map<std::string, option_bag, option_string_hash,
                     option_string_equal>
      impl_specific{};
  /// Transfer-trim knobs for bridge or history shaping.
  transfer_trim_options transfer_trim{};
  /// Budget and circuit-breaker knobs.
  call_budget_options budget{};
};

/// Materialized options for one concrete agent or tool call site.
struct resolved_call_options {
  /// Visible option values at the concrete call site.
  option_bag values{};
  /// Checkpoint-visible fields after layer overlay.
  option_bag checkpoint_fields{};
  /// Implementation-specific namespaces after layer overlay.
  std::unordered_map<std::string, option_bag, option_string_hash,
                     option_string_equal>
      impl_specific{};
  /// Concrete transfer-trim decisions.
  resolved_transfer_trim_options transfer_trim{};
  /// Concrete budget overlay.
  call_budget_options budget{};
};

/// Stores one typed option into an option bag.
template <typename key_t, typename value_t>
  requires std::constructible_from<std::string, key_t &&>
inline auto set_option(option_bag &bag, key_t &&key, value_t &&value)
    -> wh::core::result<void> {
  std::string stored_key{std::forward<key_t>(key)};
  using stored_t = wh::core::remove_cvref_t<value_t>;
  wh::core::any stored_value{};
  if constexpr (std::same_as<stored_t, wh::core::any>) {
    stored_value = std::forward<value_t>(value);
  } else {
    stored_value = wh::core::any{std::in_place_type<stored_t>,
                                 std::forward<value_t>(value)};
  }
  auto owned = wh::core::into_owned(std::move(stored_value));
  if (owned.has_error()) {
    return wh::core::result<void>::failure(owned.error());
  }
  bag.insert_or_assign(std::move(stored_key), std::move(owned).value());
  return {};
}

/// Reads one typed option by copy from an option bag.
template <typename value_t>
[[nodiscard]] inline auto option_value_copy(const option_bag &bag,
                                            const std::string_view key)
    -> wh::core::result<value_t> {
  const auto iter = bag.find(key);
  if (iter == bag.end()) {
    return wh::core::result<value_t>::failure(wh::core::errc::not_found);
  }
  const auto *typed = wh::core::any_cast<value_t>(&iter->second);
  if (typed == nullptr) {
    return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
  }
  return *typed;
}

/// Stores one global option.
template <typename key_t, typename value_t>
inline auto set_global_option(call_options &options, key_t &&key,
                              value_t &&value) -> wh::core::result<void> {
  return set_option(options.global, std::forward<key_t>(key),
                    std::forward<value_t>(value));
}

/// Stores one checkpoint-visible field.
template <typename key_t, typename value_t>
inline auto set_checkpoint_field(call_options &options, key_t &&key,
                                 value_t &&value) -> wh::core::result<void> {
  return set_option(options.checkpoint_fields, std::forward<key_t>(key),
                    std::forward<value_t>(value));
}

namespace detail {

inline auto find_named_scope(std::vector<named_option_bag> &scopes,
                             const std::string_view name)
    -> named_option_bag * {
  for (auto &scope : scopes) {
    if (scope.name == name) {
      return std::addressof(scope);
    }
  }
  return nullptr;
}

inline auto find_named_scope(const std::vector<named_option_bag> &scopes,
                             const std::string_view name)
    -> const named_option_bag * {
  for (const auto &scope : scopes) {
    if (scope.name == name) {
      return std::addressof(scope);
    }
  }
  return nullptr;
}

inline auto upsert_scope(std::vector<named_option_bag> &scopes,
                         const std::string_view name) -> named_option_bag & {
  if (auto *existing = find_named_scope(scopes, name); existing != nullptr) {
    return *existing;
  }
  scopes.push_back(named_option_bag{.name = std::string{name}});
  return scopes.back();
}

inline auto merge_option_bag(option_bag &target, const option_bag &next)
    -> void {
  for (const auto &[key, value] : next) {
    target.insert_or_assign(key, value);
  }
}

inline auto merge_named_scopes(std::vector<named_option_bag> &target,
                               const std::vector<named_option_bag> &next)
    -> void {
  for (const auto &scope : next) {
    auto &materialized = upsert_scope(target, scope.name);
    merge_option_bag(materialized.values, scope.values);
  }
}

inline auto merge_impl_specific(
    std::unordered_map<std::string, option_bag, option_string_hash,
                       option_string_equal> &target,
    const std::unordered_map<std::string, option_bag, option_string_hash,
                             option_string_equal> &next) -> void {
  for (const auto &[key, values] : next) {
    merge_option_bag(target[key], values);
  }
}

inline auto overlay_budget(call_budget_options &target,
                           const call_budget_options &next) -> void {
  if (next.max_concurrency.has_value()) {
    target.max_concurrency = next.max_concurrency;
  }
  if (next.max_iterations.has_value()) {
    target.max_iterations = next.max_iterations;
  }
  if (next.token_budget.has_value()) {
    target.token_budget = next.token_budget;
  }
  if (next.circuit_breaker_threshold.has_value()) {
    target.circuit_breaker_threshold = next.circuit_breaker_threshold;
  }
  if (next.timeout.has_value()) {
    target.timeout = next.timeout;
  }
  if (next.fail_fast.has_value()) {
    target.fail_fast = next.fail_fast;
  }
}

inline auto overlay_transfer_trim(transfer_trim_options &target,
                                  const transfer_trim_options &next) -> void {
  if (next.trim_assistant_transfer_message.has_value()) {
    target.trim_assistant_transfer_message =
        next.trim_assistant_transfer_message;
  }
  if (next.trim_tool_transfer_pair.has_value()) {
    target.trim_tool_transfer_pair = next.trim_tool_transfer_pair;
  }
}

inline auto materialize_resolved_trim(const transfer_trim_options &trim)
    -> resolved_transfer_trim_options {
  return resolved_transfer_trim_options{
      .trim_assistant_transfer_message =
          trim.trim_assistant_transfer_message.value_or(false),
      .trim_tool_transfer_pair = trim.trim_tool_transfer_pair.value_or(false),
  };
}

} // namespace detail

/// Stores one agent-targeted option.
template <typename key_t, typename value_t>
inline auto set_agent_option(call_options &options,
                             const std::string_view agent_name, key_t &&key,
                             value_t &&value) -> wh::core::result<void> {
  auto &scope = detail::upsert_scope(options.agent_scopes, agent_name);
  return set_option(scope.values, std::forward<key_t>(key),
                    std::forward<value_t>(value));
}

/// Stores one tool-targeted option.
template <typename key_t, typename value_t>
inline auto set_tool_option(call_options &options,
                            const std::string_view tool_name, key_t &&key,
                            value_t &&value) -> wh::core::result<void> {
  auto &scope = detail::upsert_scope(options.tool_scopes, tool_name);
  return set_option(scope.values, std::forward<key_t>(key),
                    std::forward<value_t>(value));
}

/// Stores one implementation-specific option.
template <typename key_t, typename value_t>
inline auto set_impl_option(call_options &options,
                            const std::string_view impl_name, key_t &&key,
                            value_t &&value) -> wh::core::result<void> {
  return set_option(options.impl_specific[std::string{impl_name}],
                    std::forward<key_t>(key), std::forward<value_t>(value));
}

/// Overlays four option layers in the fixed order:
/// defaults -> workflow facade -> adk -> call override.
[[nodiscard]] inline auto resolve_call_options(
    const call_options *defaults = nullptr,
    const call_options *workflow = nullptr, const call_options *adk = nullptr,
    const call_options *call_override = nullptr) -> call_options {
  call_options resolved{};
  const std::array<const call_options *, 4U> layers{defaults, workflow, adk,
                                                    call_override};
  for (const auto *layer : layers) {
    if (layer == nullptr) {
      continue;
    }
    detail::merge_option_bag(resolved.global, layer->global);
    detail::merge_option_bag(resolved.checkpoint_fields,
                             layer->checkpoint_fields);
    detail::merge_named_scopes(resolved.agent_scopes, layer->agent_scopes);
    detail::merge_named_scopes(resolved.tool_scopes, layer->tool_scopes);
    detail::merge_impl_specific(resolved.impl_specific, layer->impl_specific);
    detail::overlay_transfer_trim(resolved.transfer_trim, layer->transfer_trim);
    detail::overlay_budget(resolved.budget, layer->budget);
  }
  return resolved;
}

/// Materializes the option view visible to one concrete agent.
[[nodiscard]] inline auto
materialize_agent_scope(const call_options &options,
                        const std::string_view agent_name)
    -> resolved_call_options {
  resolved_call_options resolved{};
  detail::merge_option_bag(resolved.values, options.global);
  if (const auto *scope =
          detail::find_named_scope(options.agent_scopes, agent_name);
      scope != nullptr) {
    detail::merge_option_bag(resolved.values, scope->values);
  }
  detail::merge_option_bag(resolved.checkpoint_fields,
                           options.checkpoint_fields);
  resolved.impl_specific = options.impl_specific;
  resolved.transfer_trim =
      detail::materialize_resolved_trim(options.transfer_trim);
  resolved.budget = options.budget;
  return resolved;
}

/// Materializes the option view visible to one concrete tool.
[[nodiscard]] inline auto
materialize_tool_scope(const call_options &options,
                       const std::string_view tool_name)
    -> resolved_call_options {
  resolved_call_options resolved{};
  detail::merge_option_bag(resolved.values, options.global);
  if (const auto *scope =
          detail::find_named_scope(options.tool_scopes, tool_name);
      scope != nullptr) {
    detail::merge_option_bag(resolved.values, scope->values);
  }
  detail::merge_option_bag(resolved.checkpoint_fields,
                           options.checkpoint_fields);
  resolved.impl_specific = options.impl_specific;
  resolved.transfer_trim =
      detail::materialize_resolved_trim(options.transfer_trim);
  resolved.budget = options.budget;
  return resolved;
}

} // namespace wh::adk
