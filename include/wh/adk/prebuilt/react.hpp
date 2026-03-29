// Defines the thin prebuilt ReAct wrapper that forwards authoring operations to
// the shared chat-model-agent lowering helper.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "wh/adk/chatmodel.hpp"
#include "wh/model/chat_model.hpp"

namespace wh::adk::prebuilt {

/// Thin ReAct scenario wrapper that reuses `chat_model_agent` as the only
/// execution kernel.
template <wh::model::chat_model_like model_t>
class react {
public:
  /// Creates one prebuilt ReAct wrapper from the required name, description,
  /// and model.
  react(std::string name, std::string description, model_t model) noexcept
      : agent_(std::move(name), std::move(description), std::move(model)) {}

  react(const react &) = delete;
  auto operator=(const react &) -> react & = delete;
  react(react &&) noexcept = default;
  auto operator=(react &&) noexcept -> react & = default;
  ~react() = default;

  /// Returns the stable authored scenario name.
  [[nodiscard]] auto name() const noexcept -> std::string_view {
    return agent_.name();
  }

  /// Returns the human-readable scenario description.
  [[nodiscard]] auto description() const noexcept -> std::string_view {
    return agent_.description();
  }

  /// Returns true after the wrapped agent has frozen successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return agent_.frozen(); }

  /// Appends one instruction fragment before freeze.
  auto append_instruction(std::string text, const std::int32_t priority = 0)
      -> wh::core::result<void> {
    return agent_.append_instruction(std::move(text), priority);
  }

  /// Replaces the current base instruction before freeze.
  auto replace_instruction(std::string text, const std::int32_t priority = 0)
      -> wh::core::result<void> {
    return agent_.replace_instruction(std::move(text), priority);
  }

  /// Registers one executable tool component before freeze.
  template <wh::agent::detail::registered_tool_component tool_t>
  auto add_tool(const tool_t &tool,
                const wh::agent::tool_registration registration = {})
      -> wh::core::result<void> {
    return agent_.add_tool(tool, registration);
  }

  /// Registers one raw compose tool entry before freeze.
  auto add_tool_entry(wh::schema::tool_schema_definition schema,
                      wh::compose::tool_entry entry,
                      const wh::agent::tool_registration registration = {})
      -> wh::core::result<void> {
    return agent_.add_tool_entry(std::move(schema), std::move(entry), registration);
  }

  /// Appends one tool middleware layer before freeze.
  auto add_tool_middleware(wh::compose::tool_middleware middleware)
      -> wh::core::result<void> {
    return agent_.add_tool_middleware(std::move(middleware));
  }

  /// Appends one model-turn middleware layer before freeze.
  auto add_middleware(chat_model_agent_middleware middleware)
      -> wh::core::result<void> {
    return agent_.add_middleware(std::move(middleware));
  }

  /// Replaces the maximum iteration budget. `0` falls back to the safe default.
  auto set_max_iterations(const std::size_t max_iterations)
      -> wh::core::result<void> {
    return agent_.set_max_iterations(max_iterations);
  }

  /// Enables or disables forwarding of intermediate assistant/tool events.
  auto set_emit_internal_events(const bool enabled) -> wh::core::result<void> {
    return agent_.set_emit_internal_events(enabled);
  }

  /// Sets the optional output slot name written into `react_state::output_values`.
  auto set_output_key(std::string output_key) -> wh::core::result<void> {
    return agent_.set_output_key(std::move(output_key));
  }

  /// Sets whether the configured output key stores the final message or the
  /// final rendered text.
  auto set_output_mode(const wh::agent::react_output_mode mode)
      -> wh::core::result<void> {
    return agent_.set_output_mode(mode);
  }

  /// Freezes the wrapped ReAct lowering once.
  auto freeze() -> wh::core::result<void> { return agent_.freeze(); }

  /// Runs one full ReAct loop and returns the produced ADK events plus the
  /// final state/report.
  auto run(const wh::model::chat_request &request, wh::core::run_context &context)
      -> auto {
    return agent_.run(request, context);
  }

  /// Runs one full ReAct loop from a movable request payload.
  auto run(wh::model::chat_request &&request, wh::core::run_context &context)
      -> auto {
    return agent_.run(std::move(request), context);
  }

  /// Returns the wrapped lowering helper for tests and advanced composition.
  [[nodiscard]] auto agent() noexcept -> chat_model_agent<model_t> & {
    return agent_;
  }

  /// Returns the wrapped lowering helper for tests and advanced composition.
  [[nodiscard]] auto agent() const noexcept -> const chat_model_agent<model_t> & {
    return agent_;
  }

private:
  /// Shared ReAct lowering helper reused by this prebuilt wrapper.
  chat_model_agent<model_t> agent_;
};

} // namespace wh::adk::prebuilt
