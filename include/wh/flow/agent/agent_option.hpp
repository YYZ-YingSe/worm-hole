// Defines flow-level agent option wrappers and tool-binding helpers.
#pragma once

#include <array>
#include <string>
#include <utility>

#include "wh/adk/call_options.hpp"
#include "wh/agent/toolset.hpp"
#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/tool.hpp"

namespace wh::flow::agent {

/// Structured-output strategy exposed by flow-level wrappers.
enum class structured_output_strategy {
  /// Prefer provider-native structured output.
  provider = 0U,
  /// Prefer tool-call mediated structured output.
  tool,
  /// Negotiate automatically from model/tool capabilities.
  automatic,
};

/// Stable pair of model-visible declaration and runtime execution binding.
struct tool_binding_pair {
  /// Public schema bound into the model request.
  wh::schema::tool_schema_definition schema{};
  /// Runtime execution entry bound into the tools node.
  wh::compose::tool_entry entry{};
};

/// Flow-level option shell that explicitly separates compose controls from ADK
/// option layers.
struct agent_options {
  /// Compose-level typed controls.
  wh::compose::graph_call_options compose_controls{};
  /// Flow-level ADK option layer.
  wh::adk::call_options flow_controls{};
  /// ADK-level option layer.
  wh::adk::call_options adk_controls{};
  /// Per-call override layer.
  wh::adk::call_options call_override{};
  /// Structured-output strategy bit exposed to flow wrappers.
  structured_output_strategy structured_output{
      structured_output_strategy::automatic};
  /// True injects the default history rewriter when callers do not provide one.
  bool inject_default_history_rewriter{true};
};

/// Resolves the fixed option overlay order:
/// defaults -> flow -> adk -> call override.
[[nodiscard]] inline auto resolve_agent_call_options(
    const wh::adk::call_options *defaults, const agent_options &options)
    -> wh::adk::call_options {
  return wh::adk::resolve_call_options(defaults, &options.flow_controls,
                                       &options.adk_controls,
                                       &options.call_override);
}

template <wh::agent::detail::registered_tool_component tool_t>
[[nodiscard]] inline auto make_tool_binding_pair(
    const tool_t &tool, const wh::agent::tool_registration registration = {})
    -> tool_binding_pair {
  return tool_binding_pair{
      .schema = tool.schema(),
      .entry = wh::agent::detail::make_tool_entry(tool, registration.return_direct),
  };
}

template <wh::model::chat_model_like model_t>
[[nodiscard]] inline auto bind_model_tools(
    const model_t &model,
    const std::span<const wh::schema::tool_schema_definition> tools)
    -> wh::core::result<model_t> {
  if (tools.empty()) {
    return model;
  }
  return model.bind_tools(tools);
}

} // namespace wh::flow::agent
