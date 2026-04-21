// Defines compile-resolved edge-lowering kinds used by compose graph internals.
#pragma once

#include <cstdint>
#include <string_view>

namespace wh::compose {

/// Compile-resolved edge-lowering families used after authoring is frozen.
enum class edge_lowering_kind : std::uint8_t {
  /// Source and target contracts already match; payload passes through unchanged.
  none = 0U,
  /// Builtin lowering that lifts one value payload into one stream payload.
  value_to_stream,
  /// Builtin lowering that collects one stream payload into one value payload.
  stream_to_value,
  /// Compile-selected custom lowering.
  custom,
};

/// Returns a stable string label for one compile-resolved edge lowering.
[[nodiscard]] constexpr auto to_string(const edge_lowering_kind kind) noexcept -> std::string_view {
  switch (kind) {
  case edge_lowering_kind::none:
    return "none";
  case edge_lowering_kind::value_to_stream:
    return "value_to_stream";
  case edge_lowering_kind::stream_to_value:
    return "stream_to_value";
  case edge_lowering_kind::custom:
    return "custom";
  }
  return "none";
}

} // namespace wh::compose
