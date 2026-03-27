// Defines borrowed per-call tool metadata passed through tools-node dispatch.
#pragma once

#include <string_view>

#include "wh/core/address.hpp"
#include "wh/core/run_context.hpp"

namespace wh::tool {

/// Borrowed runtime scope for one concrete tool call.
struct call_scope {
  /// Base run context shared with the current graph invocation.
  wh::core::run_context &run;
  /// Stable component label that initiated the tool call.
  std::string_view component;
  /// Stable implementation label for the dispatch endpoint.
  std::string_view implementation;
  /// Current tool name.
  std::string_view tool_name;
  /// Stable correlation id for this tool call.
  std::string_view call_id;

  /// Builds structured execution location for this tool call on demand.
  [[nodiscard]] auto location() const -> wh::core::address {
    return wh::core::address{
        std::initializer_list<std::string_view>{"tool", tool_name, call_id}};
  }
};

} // namespace wh::tool
