// Defines shared agent-facing option wrappers and tool-binding helpers without
// reviving the deprecated flow facade.
#pragma once

#include <optional>
#include <span>

#include "wh/adk/call_options.hpp"
#include "wh/agent/tool_binding.hpp"
#include "wh/compose/graph/call_options.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"

namespace wh::agent {

/// Structured-output strategy exposed by agent-family wrappers.
enum class structured_output_strategy {
  /// Prefer provider-native structured output.
  provider = 0U,
  /// Prefer tool-call mediated structured output.
  tool,
};

/// Shared authored option shell layered on top of compose and ADK controls.
struct agent_options {
  /// Compose-level typed controls.
  wh::compose::graph_call_options compose_controls{};
  /// Agent-level ADK option layer.
  wh::adk::call_options agent_controls{};
  /// ADK-level option layer.
  wh::adk::call_options adk_controls{};
  /// Per-call override layer.
  wh::adk::call_options call_override{};
  /// Optional structured-output strategy. Empty means "do not inject one".
  std::optional<structured_output_strategy> structured_output{};
};

/// Maps agent-level structured-output preference to model-layer negotiation
/// policy. There is no automatic fallback mode on the public surface.
[[nodiscard]] inline auto
resolve_structured_output_policy(const structured_output_strategy strategy) noexcept
    -> wh::model::structured_output_policy {
  switch (strategy) {
  case structured_output_strategy::provider:
    return wh::model::structured_output_policy{
        .preference = wh::model::structured_output_preference::provider_native_first,
        .allow_tool_fallback = false,
    };
  case structured_output_strategy::tool:
    return wh::model::structured_output_policy{
        .preference = wh::model::structured_output_preference::tool_call_first,
        .allow_tool_fallback = true,
    };
  }
  return wh::model::structured_output_policy{};
}

/// Projects agent-level options into the per-request chat-model override
/// channel.
inline auto apply_agent_options(wh::model::chat_request &request, const agent_options &options)
    -> void {
  if (!options.structured_output.has_value()) {
    return;
  }
  auto override = request.options.call_override().value_or(wh::model::chat_model_common_options{});
  override.structured_output = resolve_structured_output_policy(*options.structured_output);
  request.options.set_call_override(std::move(override));
}

/// Resolves layered call options in the fixed order:
/// defaults -> agent -> adk -> call override.
[[nodiscard]] inline auto resolve_agent_call_options(const wh::adk::call_options *defaults,
                                                     const agent_options &options)
    -> wh::adk::call_options {
  return wh::adk::resolve_call_options(defaults, &options.agent_controls, &options.adk_controls,
                                       &options.call_override);
}

template <typename model_t>
concept tool_bindable_model =
    requires(const model_t &model, std::span<const wh::schema::tool_schema_definition> tools) {
      { model.bind_tools(tools) } -> std::same_as<std::remove_cvref_t<model_t>>;
    };

template <tool_bindable_model model_t>
[[nodiscard]] inline auto
bind_model_tools(const model_t &model,
                 const std::span<const wh::schema::tool_schema_definition> tools)
    -> wh::core::result<model_t> {
  if (tools.empty()) {
    return model;
  }
  return model.bind_tools(tools);
}

} // namespace wh::agent
