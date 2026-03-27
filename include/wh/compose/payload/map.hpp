// Defines payload-map conversions between graph payloads and keyed value maps.
#pragma once

#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/compose/types.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::compose {

/// Converts payload into map-shaped value or returns type mismatch.
[[nodiscard]] inline auto payload_to_value_map(const graph_value &payload)
    -> wh::core::result<graph_value_map> {
  if (const auto *typed = wh::core::any_cast<graph_value_map>(&payload);
      typed != nullptr) {
    return *typed;
  }
  return wh::core::result<graph_value_map>::failure(
      wh::core::errc::type_mismatch);
}

/// Borrows map-shaped payload without copying.
[[nodiscard]] inline auto payload_to_value_map_cref(const graph_value &payload)
    -> wh::core::result<std::reference_wrapper<const graph_value_map>> {
  if (const auto *typed = wh::core::any_cast<graph_value_map>(&payload);
      typed != nullptr) {
    return std::cref(*typed);
  }
  return wh::core::result<std::reference_wrapper<const graph_value_map>>::failure(
      wh::core::errc::type_mismatch);
}

/// Moves payload into map-shaped value or returns type mismatch.
[[nodiscard]] inline auto payload_to_value_map(graph_value &&payload)
    -> wh::core::result<graph_value_map> {
  if (auto *typed = wh::core::any_cast<graph_value_map>(&payload);
      typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<graph_value_map>::failure(
      wh::core::errc::type_mismatch);
}

/// Wraps map-shaped value into payload.
[[nodiscard]] inline auto value_map_to_payload(const graph_value_map &value)
    -> graph_value {
  return graph_value{value};
}

/// Wraps map-shaped value into payload.
[[nodiscard]] inline auto value_map_to_payload(graph_value_map &&value)
    -> graph_value {
  return graph_value{std::move(value)};
}

/// Extracts one keyed payload from map-shaped value.
[[nodiscard]] inline auto extract_keyed_input(const graph_value_map &map_input,
                                              const std::string_view key)
    -> wh::core::result<graph_value> {
  const auto iter = map_input.find(key);
  if (iter == map_input.end()) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  return iter->second;
}

/// Move-enabled keyed extract that moves payload from map-shaped value.
[[nodiscard]] inline auto extract_keyed_input(graph_value_map &&map_input,
                                              const std::string_view key)
    -> wh::core::result<graph_value> {
  const auto iter = map_input.find(key);
  if (iter == map_input.end()) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  return std::move(iter->second);
}

template <typename key_t, typename value_t>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<graph_value, value_t &&>
/// Writes one keyed payload into map-shaped value.
inline auto write_keyed_output(graph_value_map &map_output, key_t &&key,
                               value_t &&value) -> void {
  graph_value payload{};
  if constexpr (std::same_as<wh::core::remove_cvref_t<value_t>, graph_value>) {
    payload = std::forward<value_t>(value);
  } else {
    payload = graph_value{std::forward<value_t>(value)};
  }
  map_output.insert_or_assign(std::string{std::forward<key_t>(key)},
                              std::move(payload));
}

} // namespace wh::compose
