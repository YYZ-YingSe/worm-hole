// Defines keyed payload helpers layered on top of compose payload maps.
#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <utility>

#include "wh/compose/payload/map.hpp"

namespace wh::compose {

/// Reads one key from payload-map input.
[[nodiscard]] inline auto keyed_input(const graph_value &input, const std::string_view key)
    -> wh::core::result<graph_value> {
  auto map_input = payload_to_value_map_cref(input);
  if (map_input.has_error()) {
    return wh::core::result<graph_value>::failure(map_input.error());
  }
  return extract_keyed_input(map_input.value().get(), key);
}

/// Reads one key from movable payload-map input.
[[nodiscard]] inline auto keyed_input(graph_value &&input, const std::string_view key)
    -> wh::core::result<graph_value> {
  auto map_input = payload_to_value_map(std::move(input));
  if (map_input.has_error()) {
    return wh::core::result<graph_value>::failure(map_input.error());
  }
  return extract_keyed_input(std::move(map_input).value(), key);
}

template <typename key_t, typename value_t>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<graph_value, value_t &&>
/// Wraps one payload as map output under target key.
[[nodiscard]] inline auto keyed_output(key_t &&key, value_t &&value) -> graph_value {
  graph_value_map output{};
  write_keyed_output(output, std::forward<key_t>(key), std::forward<value_t>(value));
  return value_map_to_payload(std::move(output));
}

/// Packs scalar payload into keyed map payload for keyed contracts.
[[nodiscard]] inline auto pack_keyed_payload(const std::string_view key, const graph_value &value)
    -> graph_value {
  graph_value_map output{};
  write_keyed_output(output, key, value);
  return value_map_to_payload(std::move(output));
}

/// Packs movable payload into keyed map payload for keyed contracts.
[[nodiscard]] inline auto pack_keyed_payload(const std::string_view key, graph_value &&value)
    -> graph_value {
  graph_value_map output{};
  write_keyed_output(output, key, std::move(value));
  return value_map_to_payload(std::move(output));
}

/// Unpacks one keyed payload from map payload.
[[nodiscard]] inline auto unpack_keyed_payload(const graph_value &payload,
                                               const std::string_view key)
    -> wh::core::result<graph_value> {
  auto map_payload = payload_to_value_map_cref(payload);
  if (map_payload.has_error()) {
    return wh::core::result<graph_value>::failure(map_payload.error());
  }
  return extract_keyed_input(map_payload.value().get(), key);
}

/// Unpacks one keyed payload from movable map payload.
[[nodiscard]] inline auto unpack_keyed_payload(graph_value &&payload, const std::string_view key)
    -> wh::core::result<graph_value> {
  auto map_payload = payload_to_value_map(std::move(payload));
  if (map_payload.has_error()) {
    return wh::core::result<graph_value>::failure(map_payload.error());
  }
  return extract_keyed_input(std::move(map_payload).value(), key);
}

} // namespace wh::compose
