// Defines shared value-input assembly operations used by DAG and Pregel runtimes.
#pragma once

#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/compose/graph/detail/runtime/common_input_types.hpp"
#include "wh/core/result.hpp"

namespace wh::compose::detail::input_runtime {

inline auto append_value_input(value_batch &batch, value_input entry) -> wh::core::result<void> {
  switch (batch.form) {
  case value_input_form::direct:
    if (batch.single.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    batch.single = std::move(entry);
    return {};
  case value_input_form::fan_in:
    batch.fan_in.push_back(std::move(entry));
    return {};
  }
  return wh::core::result<void>::failure(wh::core::errc::internal_error);
}

[[nodiscard]] inline auto materialize_value_input(value_input &entry)
    -> wh::core::result<graph_value> {
  if (entry.owned.has_value()) {
    return wh::compose::detail::materialize_value_payload(std::move(*entry.owned));
  }

  auto *value = entry.value();
  if (value == nullptr) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  return wh::compose::detail::materialize_value_payload(*value);
}

template <typename key_fn_t>
  requires std::invocable<key_fn_t &, const value_input &> &&
           std::convertible_to<std::invoke_result_t<key_fn_t &, const value_input &>,
                               std::string_view>
[[nodiscard]] inline auto build_value_input_map(std::vector<value_input> &entries,
                                                key_fn_t &&key_fn)
    -> wh::core::result<graph_value_map> {
  auto &&lookup_key = key_fn;
  graph_value_map fan_in_input{};
  fan_in_input.reserve(entries.size());
  for (auto &entry : entries) {
    auto materialized = materialize_value_input(entry);
    if (materialized.has_error()) {
      return wh::core::result<graph_value_map>::failure(materialized.error());
    }
    auto key = std::invoke(lookup_key, entry);
    fan_in_input.insert_or_assign(std::string{std::string_view{key}},
                                  std::move(materialized).value());
  }
  return fan_in_input;
}

} // namespace wh::compose::detail::input_runtime
