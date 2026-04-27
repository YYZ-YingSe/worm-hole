// Defines reusable model-visible tool binding pairs without reviving a second
// middleware runtime.
#pragma once

#include "wh/agent/toolset.hpp"

namespace wh::agent {

/// Stable pair of model-visible schema and runtime execution binding.
struct tool_binding_pair {
  /// Public schema bound into the model request.
  wh::schema::tool_schema_definition schema{};
  /// Runtime execution entry bound into the lowered tools node.
  wh::compose::tool_entry entry{};
};

template <wh::agent::detail::registered_tool_component tool_t>
[[nodiscard]] inline auto
make_tool_binding_pair(const tool_t &tool, const wh::agent::tool_registration registration = {})
    -> tool_binding_pair {
  return tool_binding_pair{
      .schema = tool.schema(),
      .entry = wh::agent::detail::make_tool_entry(tool, registration.return_direct),
  };
}

} // namespace wh::agent
