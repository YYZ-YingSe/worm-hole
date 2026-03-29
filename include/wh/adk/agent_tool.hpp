// Defines the thin agent-to-tool bridge that freezes tool metadata and lowers
// one bound child-agent runner into the existing compose tool contract.
#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/adk/event_stream.hpp"
#include "wh/adk/react.hpp"
#include "wh/adk/utils.hpp"
#include "wh/agent/agent.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/function.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/stream/reader.hpp"
#include "wh/schema/tool.hpp"
#include "wh/tool/call_scope.hpp"
#include "wh/tool/tool.hpp"

namespace wh::adk {

/// Supported authored input shapes for one future agent-tool bridge.
enum class agent_tool_input_mode {
  /// Use the default `{request:string}` tool input contract.
  request = 0U,
  /// Rehydrate one full-history request from the current ReAct state.
  message_history,
  /// Pass the raw JSON payload through as one user message.
  custom_schema,
};

/// Final bridge report returned after one agent-tool run.
struct agent_tool_result {
  /// Boundary-visible event stream after prefix projection and action filtering.
  agent_event_stream_reader events{};
  /// Ordered text chunks extracted from the child path for tool streaming.
  std::vector<std::string> output_chunks{};
  /// Flattened text returned to the tool caller.
  std::string output_text{};
  /// Final message observed on the child-agent path, when any.
  std::optional<wh::schema::message> final_message{};
  /// Terminal error observed inside the bridge, when any.
  std::optional<wh::core::error_code> final_error{};
  /// True when the child path emitted one interrupt action.
  bool interrupted{false};
};

namespace detail {

using agent_tool_runner = wh::core::callback_function<
    agent_run_result(const wh::model::chat_request &, wh::core::run_context &) const>;

template <typename value_t> struct agent_tool_runner_box {
  mutable value_t value;
};

template <typename runner_t>
concept agent_tool_runner_object =
    requires(runner_t &runner, const wh::model::chat_request &request,
             wh::core::run_context &context) {
      { runner.run(request, context) } -> std::same_as<agent_run_result>;
    };

template <typename runner_t>
concept agent_tool_runner_callable =
    requires(runner_t &runner, const wh::model::chat_request &request,
             wh::core::run_context &context) {
      { std::invoke(runner, request, context) } -> std::same_as<agent_run_result>;
    };

template <typename runner_t>
concept bindable_agent_tool_runner =
    std::copy_constructible<std::remove_cvref_t<runner_t>> &&
    (agent_tool_runner_object<std::remove_cvref_t<runner_t>> ||
     agent_tool_runner_callable<std::remove_cvref_t<runner_t>>);

template <typename runner_t>
[[nodiscard]] inline auto dispatch_agent_tool_runner(
    runner_t &runner, const wh::model::chat_request &request,
    wh::core::run_context &context) -> agent_run_result {
  if constexpr (agent_tool_runner_object<runner_t>) {
    return runner.run(request, context);
  } else {
    return std::invoke(runner, request, context);
  }
}

/// Frozen runtime bundle captured by compose tool-entry lambdas.
struct agent_tool_runtime {
  /// Stable public tool name.
  std::string tool_name{};
  /// Stable bound child-agent name.
  std::string agent_name{};
  /// Frozen input mapping mode.
  agent_tool_input_mode input_mode{agent_tool_input_mode::request};
  /// True forwards child internal events after boundary filtering.
  bool forward_internal_events{false};
  /// Frozen child-agent execution entrypoint.
  agent_tool_runner runner{nullptr};
};

/// Intermediate owned bridge payload before it is re-exposed as readers/values.
struct materialized_agent_tool_output {
  /// Boundary-visible filtered events.
  std::vector<agent_event> events{};
  /// Ordered text chunks extracted from child message events.
  std::vector<std::string> text_chunks{};
  /// Final observed message, when any.
  std::optional<wh::schema::message> final_message{};
  /// Terminal error observed inside the child event stream.
  std::optional<wh::core::error_code> final_error{};
  /// True when one interrupt action survived the tool boundary.
  bool interrupted{false};
};

[[nodiscard]] inline auto make_user_message(std::string text)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

[[nodiscard]] inline auto default_tool_metadata(
    const agent_tool_runtime &runtime, const wh::tool::call_scope &scope)
    -> event_metadata {
  return event_metadata{
      .run_path = append_run_path_prefix(
          scope.location(), run_path{{"agent", runtime.agent_name}}),
      .agent_name = runtime.agent_name,
      .tool_name = runtime.tool_name,
  };
}

[[nodiscard]] inline auto normalize_child_metadata(
    const agent_tool_runtime &runtime, const wh::tool::call_scope &scope,
    const event_metadata &source) -> event_metadata {
  auto normalized = default_tool_metadata(runtime, scope);
  if (!source.agent_name.empty()) {
    normalized.agent_name = source.agent_name;
    normalized.run_path = append_run_path_prefix(
        scope.location(), run_path{{"agent", source.agent_name}});
  }
  if (!source.tool_name.empty()) {
    normalized.tool_name = source.tool_name;
  }
  return normalized;
}

[[nodiscard]] inline auto parse_request_text(const std::string_view input_json)
    -> wh::core::result<std::string> {
  auto parsed = wh::core::parse_json(input_json);
  if (parsed.has_error()) {
    return wh::core::result<std::string>::failure(parsed.error());
  }
  auto request_value = wh::core::json_find_member(parsed.value(), "request");
  if (request_value.has_error()) {
    return wh::core::result<std::string>::failure(request_value.error());
  }
  if (!request_value.value()->IsString()) {
    return wh::core::result<std::string>::failure(wh::core::errc::type_mismatch);
  }
  return std::string{request_value.value()->GetString(),
                     static_cast<std::size_t>(request_value.value()->GetStringLength())};
}

[[nodiscard]] inline auto build_history_request(
    const wh::compose::tool_call &call)
    -> wh::core::result<wh::model::chat_request> {
  if (!call.payload.has_value()) {
    return wh::core::result<wh::model::chat_request>::failure(
        wh::core::errc::not_found);
  }
  if (const auto *request =
          wh::core::any_cast<wh::model::chat_request>(&call.payload);
      request != nullptr) {
    return *request;
  }
  return wh::core::result<wh::model::chat_request>::failure(
      wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto build_agent_tool_request(
    const agent_tool_runtime &runtime, const wh::compose::tool_call &call,
    const wh::core::run_context &context)
    -> wh::core::result<wh::model::chat_request> {
  switch (runtime.input_mode) {
  case agent_tool_input_mode::request: {
    auto request_text = parse_request_text(call.arguments);
    if (request_text.has_error()) {
      return wh::core::result<wh::model::chat_request>::failure(
          request_text.error());
    }
    wh::model::chat_request request{};
    request.messages.push_back(make_user_message(std::move(request_text).value()));
    return request;
  }
  case agent_tool_input_mode::message_history: {
    static_cast<void>(context);
    return build_history_request(call);
  }
  case agent_tool_input_mode::custom_schema: {
    wh::model::chat_request request{};
    request.messages.push_back(make_user_message(call.arguments));
    return request;
  }
  }
  return wh::core::result<wh::model::chat_request>::failure(
      wh::core::errc::not_supported);
}

[[nodiscard]] inline auto materialize_agent_tool_output(
    const agent_tool_runtime &runtime, agent_run_output artifact,
    const wh::tool::call_scope &scope)
    -> wh::core::result<materialized_agent_tool_output> {
  auto events = collect_agent_events(std::move(artifact.events));
  if (events.has_error()) {
    return wh::core::result<materialized_agent_tool_output>::failure(
        events.error());
  }

  materialized_agent_tool_output output{};
  output.events.reserve(events.value().size() + 1U);
  for (auto &event : events.value()) {
    auto prefixed = prefix_agent_event(std::move(event), scope.location());
    auto normalized_metadata =
        normalize_child_metadata(runtime, scope, prefixed.metadata);
    if (auto *message = std::get_if<message_event>(&prefixed.payload);
        message != nullptr) {
      auto snapshots = snapshot_message_event(std::move(*message));
      if (snapshots.has_error()) {
        return wh::core::result<materialized_agent_tool_output>::failure(
            snapshots.error());
      }
      for (auto &entry : snapshots.value()) {
        auto text = render_message_text(entry);
        if (!text.empty()) {
          output.text_chunks.push_back(std::move(text));
        }
        output.final_message = entry;
        if (runtime.forward_internal_events) {
          output.events.push_back(
              make_message_event(std::move(entry), normalized_metadata));
        }
      }
      continue;
    }

    if (const auto *action = std::get_if<control_action>(&prefixed.payload);
        action != nullptr) {
      if (action->kind == control_action_kind::interrupt) {
        output.interrupted = true;
        output.events.push_back(
            make_control_event(*action, std::move(normalized_metadata)));
      }
      continue;
    }

    if (const auto *error = std::get_if<error_event>(&prefixed.payload);
        error != nullptr) {
      output.final_error = error->code;
      output.events.push_back(make_error_event(
          error->code, error->message, error->detail, std::move(normalized_metadata)));
      continue;
    }

    if (runtime.forward_internal_events) {
      if (const auto *custom = std::get_if<custom_event>(&prefixed.payload);
          custom != nullptr) {
        output.events.push_back(make_custom_event(custom->name, custom->payload,
                                                  std::move(normalized_metadata)));
      }
    }
  }

  if (!output.final_message.has_value() && artifact.final_message.has_value()) {
    output.final_message = std::move(artifact.final_message);
    auto text = render_message_text(*output.final_message);
    if (!text.empty()) {
      output.text_chunks.push_back(std::move(text));
    }
  }

  if (!runtime.forward_internal_events && output.final_message.has_value()) {
    output.events.push_back(
        make_message_event(*output.final_message, default_tool_metadata(runtime, scope)));
  }

  if (output.events.empty() && output.text_chunks.empty() &&
      !output.final_error.has_value() && !output.interrupted) {
    return wh::core::result<materialized_agent_tool_output>::failure(
        wh::core::errc::protocol_error);
  }

  return output;
}

[[nodiscard]] inline auto run_agent_tool(
    const agent_tool_runtime &runtime, const wh::compose::tool_call &call,
    const wh::tool::call_scope &scope) -> wh::core::result<agent_tool_result> {
  auto request = build_agent_tool_request(runtime, call, scope.run);
  if (request.has_error()) {
    return wh::core::result<agent_tool_result>::failure(request.error());
  }

  auto run_result = runtime.runner(request.value(), scope.run);
  if (run_result.has_error()) {
    std::vector<agent_event> events{};
    events.push_back(make_error_event(run_result.error(),
                                      "agent tool bridge failed", {},
                                      default_tool_metadata(runtime, scope)));
    return agent_tool_result{
        .events = agent_event_stream_reader{
            wh::schema::stream::make_values_stream_reader(std::move(events))},
        .final_error = run_result.error(),
    };
  }

  auto output =
      materialize_agent_tool_output(runtime, std::move(run_result).value(), scope);
  if (output.has_error()) {
    return wh::core::result<agent_tool_result>::failure(output.error());
  }

  auto owned_output = std::move(output).value();
  std::string joined_text{};
  for (const auto &chunk : owned_output.text_chunks) {
    joined_text.append(chunk);
  }

  return agent_tool_result{
      .events = agent_event_stream_reader{
          wh::schema::stream::make_values_stream_reader(
              std::move(owned_output.events))},
      .output_chunks = std::move(owned_output.text_chunks),
      .output_text = std::move(joined_text),
      .final_message = std::move(owned_output.final_message),
      .final_error = owned_output.final_error,
      .interrupted = owned_output.interrupted,
  };
}

[[nodiscard]] inline auto stream_from_chunks(std::vector<std::string> chunks)
    -> wh::tool::tool_output_stream_result {
  return wh::tool::tool_output_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(chunks))};
}

} // namespace detail

/// Thin authored agent-tool bridge with frozen schema selection and runtime
/// lowering into one compose tool entry.
class agent_tool {
public:
  /// Creates one agent-tool bridge from metadata and one bound authored agent.
  agent_tool(std::string name, std::string description,
             wh::agent::agent &&bound_agent) noexcept
      : name_(std::move(name)), description_(std::move(description)),
        bound_agent_(std::move(bound_agent)) {}

  agent_tool(const agent_tool &) = delete;
  auto operator=(const agent_tool &) -> agent_tool & = delete;
  agent_tool(agent_tool &&) noexcept = default;
  auto operator=(agent_tool &&) noexcept -> agent_tool & = default;
  ~agent_tool() = default;

  /// Returns the stable tool name exposed to model/tool routing.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns the human-readable tool description.
  [[nodiscard]] auto description() const noexcept -> std::string_view {
    return description_;
  }

  /// Returns the authored input mode selected for this bridge.
  [[nodiscard]] auto input_mode() const noexcept -> agent_tool_input_mode {
    return input_mode_;
  }

  /// Returns true after metadata, schema mode, and agent binding freeze.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Returns true when internal events should be forwarded across the bridge.
  [[nodiscard]] auto forward_internal_events() const noexcept -> bool {
    return forward_internal_events_;
  }

  /// Sets the authored input mode before freeze.
  auto set_input_mode(const agent_tool_input_mode input_mode)
      -> wh::core::result<void> {
    auto mutable_result = ensure_mutable();
    if (mutable_result.has_error()) {
      return mutable_result;
    }
    input_mode_ = input_mode;
    return {};
  }

  /// Replaces the custom schema payload before freeze.
  auto set_custom_schema(wh::schema::tool_schema_definition schema)
      -> wh::core::result<void> {
    auto mutable_result = ensure_mutable();
    if (mutable_result.has_error()) {
      return mutable_result;
    }
    custom_schema_ = std::move(schema);
    return {};
  }

  /// Enables or disables authored internal-event forwarding before freeze.
  auto set_forward_internal_events(const bool enabled)
      -> wh::core::result<void> {
    auto mutable_result = ensure_mutable();
    if (mutable_result.has_error()) {
      return mutable_result;
    }
    forward_internal_events_ = enabled;
    return {};
  }

  /// Returns the bound authored agent that will later be lowered behind the
  /// tool boundary.
  [[nodiscard]] auto bound_agent() const noexcept -> const wh::agent::agent & {
    return bound_agent_;
  }

  /// Binds one executable child runner by the authored bound-agent name.
  auto bind_runner(detail::agent_tool_runner runner) -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (!static_cast<bool>(runner)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    runner_ = std::move(runner);
    return {};
  }

  /// Binds one executable child runner from a callable or `.run(...)` object.
  template <detail::bindable_agent_tool_runner runner_t>
  auto bind_runner(runner_t &&runner) -> wh::core::result<void> {
    using stored_runner_t = std::remove_cvref_t<runner_t>;
    return bind_runner(detail::agent_tool_runner{
        [runner_box = detail::agent_tool_runner_box<stored_runner_t>{
             stored_runner_t{std::forward<runner_t>(runner)}}](
            const wh::model::chat_request &request,
            wh::core::run_context &context) -> agent_run_result {
          return detail::dispatch_agent_tool_runner(runner_box.value, request,
                                                    context);
        }});
  }

  /// Materializes the authored tool schema visible to runtime/tool routing.
  [[nodiscard]] auto tool_schema() const -> wh::schema::tool_schema_definition {
    wh::schema::tool_schema_definition schema{};
    schema.name = name_;
    schema.description = description_;
    switch (input_mode_) {
    case agent_tool_input_mode::request:
      schema.parameters.push_back(wh::schema::tool_parameter_schema{
          .name = "request",
          .type = wh::schema::tool_parameter_type::string,
          .description = "tool request text",
          .required = true,
      });
      return schema;
    case agent_tool_input_mode::message_history:
      schema.raw_parameters_json_schema =
          R"({"type":"object","properties":{"messages":{"type":"array","items":{"type":"object"}}},"required":["messages"]})";
      return schema;
    case agent_tool_input_mode::custom_schema:
      if (custom_schema_.has_value()) {
        schema.parameters = custom_schema_->parameters;
        schema.raw_parameters_json_schema =
            custom_schema_->raw_parameters_json_schema;
      }
      return schema;
    }
    return schema;
  }

  /// Runs one concrete tool call through the bound child-agent runner.
  auto run(const wh::compose::tool_call &call, const wh::tool::call_scope &scope) const
      -> wh::core::result<agent_tool_result> {
    auto runtime = make_runtime();
    if (runtime.has_error()) {
      return wh::core::result<agent_tool_result>::failure(runtime.error());
    }
    return detail::run_agent_tool(runtime.value(), call, scope);
  }

  /// Lowers the bridge into one compose tool entry.
  [[nodiscard]] auto compose_entry() const -> wh::core::result<wh::compose::tool_entry> {
    auto runtime = make_runtime();
    if (runtime.has_error()) {
      return wh::core::result<wh::compose::tool_entry>::failure(runtime.error());
    }

    wh::compose::tool_entry entry{};
    entry.invoke = wh::compose::tool_invoke{
        [runtime = runtime.value()](
            const wh::compose::tool_call &call,
            wh::tool::call_scope scope) -> wh::core::result<wh::compose::graph_value> {
          auto result = detail::run_agent_tool(runtime, call, scope);
          if (result.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                result.error());
          }
          auto owned_result = std::move(result).value();
          if (owned_result.final_error.has_value()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                *owned_result.final_error);
          }
          if (owned_result.interrupted) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::canceled);
          }
          if (owned_result.output_text.empty()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::protocol_error);
          }
          return wh::compose::graph_value{std::move(owned_result.output_text)};
        }};
    entry.stream = wh::compose::tool_stream{
        [runtime = runtime.value()](
            const wh::compose::tool_call &call,
            wh::tool::call_scope scope) -> wh::core::result<wh::compose::graph_stream_reader> {
          auto result = detail::run_agent_tool(runtime, call, scope);
          if (result.has_error()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(
                result.error());
          }
          auto owned_result = std::move(result).value();
          if (owned_result.final_error.has_value()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(
                *owned_result.final_error);
          }
          if (owned_result.interrupted) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(
                wh::core::errc::canceled);
          }
          auto chunks = std::move(owned_result.output_chunks);
          if (chunks.empty() && !owned_result.output_text.empty()) {
            chunks.push_back(std::move(owned_result.output_text));
          }
          if (chunks.empty()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(
                wh::core::errc::protocol_error);
          }
          return wh::compose::to_graph_stream_reader(
              detail::stream_from_chunks(std::move(chunks)).value());
        }};
    return entry;
  }

  /// Freezes schema selection, bound agent, and bridge runner.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || description_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (input_mode_ == agent_tool_input_mode::custom_schema &&
        !custom_schema_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    auto agent_frozen = bound_agent_.freeze();
    if (agent_frozen.has_error()) {
      return agent_frozen;
    }
    frozen_ = true;
    return {};
  }

private:
  /// Rejects bridge mutation after freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Freezes metadata and returns one self-contained runtime bundle.
  [[nodiscard]] auto make_runtime() const
      -> wh::core::result<detail::agent_tool_runtime> {
    auto mutable_self = const_cast<agent_tool *>(this);
    auto frozen = mutable_self->freeze();
    if (frozen.has_error()) {
      return wh::core::result<detail::agent_tool_runtime>::failure(frozen.error());
    }
    if (!static_cast<bool>(runner_)) {
      return wh::core::result<detail::agent_tool_runtime>::failure(
          wh::core::errc::not_found);
    }
    return detail::agent_tool_runtime{
        .tool_name = name_,
        .agent_name = std::string{bound_agent_.name()},
        .input_mode = input_mode_,
        .forward_internal_events = forward_internal_events_,
        .runner = runner_,
    };
  }

  /// Stable tool name exposed to model/tool routing.
  std::string name_{};
  /// Human-readable bridge description.
  std::string description_{};
  /// Authored input mode selected for this bridge.
  agent_tool_input_mode input_mode_{agent_tool_input_mode::request};
  /// Optional caller-provided custom schema.
  std::optional<wh::schema::tool_schema_definition> custom_schema_{};
  /// True when internal events should later be forwarded across the bridge.
  bool forward_internal_events_{false};
  /// Bound authored agent that will later be lowered behind the tool boundary.
  mutable wh::agent::agent bound_agent_{""};
  /// Frozen child-agent execution entrypoint.
  detail::agent_tool_runner runner_{nullptr};
  /// True after bridge metadata has been frozen successfully.
  bool frozen_{false};
};

} // namespace wh::adk
