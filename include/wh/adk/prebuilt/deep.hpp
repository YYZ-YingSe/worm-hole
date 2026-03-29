// Defines the prebuilt deep-task scenario wrapper that reuses the shared ReAct
// lowering and injects task tools only when task subagents are available.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/adk/prebuilt/react.hpp"
#include "wh/adk/utils.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/flow/agent/utils.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/serialization/registry.hpp"
#include "wh/schema/tool.hpp"

namespace wh::adk::prebuilt {

/// One todo item tracked by the deep-task wrapper.
struct deep_todo {
  /// Stable task description recorded by the scenario.
  std::string content{};
  /// Stable task status: pending / in_progress / completed.
  std::string status{"pending"};
};

/// Final mutable state emitted by one deep-task run.
struct deep_state {
  /// Ordered todo list last written by the builtin todo tool.
  std::vector<deep_todo> todos{};
  /// Final rendered text returned by the wrapped agent when available.
  std::optional<std::string> final_text{};
  /// Named final outputs materialized for callers.
  std::unordered_map<std::string, std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      output_values{};
};

/// Immutable report emitted by one deep-task run.
struct deep_report {
  /// Ordered subagent types invoked during the run.
  std::vector<std::string> invoked_subagents{};
  /// Terminal error surfaced by the wrapped agent, when any.
  std::optional<wh::core::error_code> final_error{};
  /// True when the wrapped agent completed successfully.
  bool completed{false};
  /// True when the task tool was injected for this run.
  bool task_tool_injected{false};
};

/// Final result returned by one deep-task run.
struct deep_result {
  /// Event stream emitted by the wrapped scenario.
  agent_event_stream_reader events{};
  /// Final mutable state.
  deep_state state{};
  /// Immutable execution report.
  deep_report report{};
};

/// Frozen authoring options for one deep-task wrapper.
struct deep_options {
  /// Maximum ReAct iterations forwarded to the wrapped agent.
  std::size_t max_iterations{20U};
  /// Optional final-output slot name written into `state.output_values`.
  std::string output_key{};
  /// True forwards intermediate ReAct events into the public stream.
  bool emit_internal_events{true};
  /// True injects the builtin todo writer when task tools are available.
  bool enable_write_todos{true};
  /// Stable task tool name.
  std::string task_tool_name{"task"};
  /// Stable todo writer tool name.
  std::string todo_tool_name{"write_todos"};
};

namespace detail {

using deep_subagent_invoke = wh::core::callback_function<
    wh::flow::agent::message_result(std::string_view, wh::core::run_context &) const>;

struct deep_subagent_entry {
  /// Stable subagent type routed by the task tool.
  std::string type{};
  /// Human-readable description used for prompts and tests.
  std::string description{};
  /// Invocation entry executed when the task tool selects this type.
  deep_subagent_invoke invoke{nullptr};
};

[[nodiscard]] inline auto
parse_deep_task_arguments(const std::string_view input_json)
    -> wh::core::result<std::pair<std::string, std::string>> {
  auto parsed = wh::core::parse_json(input_json);
  if (parsed.has_error()) {
    return wh::core::result<std::pair<std::string, std::string>>::failure(
        parsed.error());
  }

  const auto subagent_type = wh::core::json_find_member(parsed.value(), "subagent_type");
  const auto description = wh::core::json_find_member(parsed.value(), "description");
  if (subagent_type.has_error() || description.has_error() ||
      !subagent_type.value()->IsString() || !description.value()->IsString()) {
    return wh::core::result<std::pair<std::string, std::string>>::failure(
        wh::core::errc::invalid_argument);
  }

  return std::pair<std::string, std::string>{
      std::string{subagent_type.value()->GetString(),
                  static_cast<std::size_t>(subagent_type.value()->GetStringLength())},
      std::string{description.value()->GetString(),
                  static_cast<std::size_t>(description.value()->GetStringLength())},
  };
}

[[nodiscard]] inline auto parse_deep_todos(const std::string_view input_json)
    -> wh::core::result<std::vector<deep_todo>> {
  auto parsed = wh::core::parse_json(input_json);
  if (parsed.has_error()) {
    return wh::core::result<std::vector<deep_todo>>::failure(parsed.error());
  }

  const auto todos = wh::core::json_find_member(parsed.value(), "todos");
  if (todos.has_error() || !todos.value()->IsArray()) {
    return wh::core::result<std::vector<deep_todo>>::failure(
        wh::core::errc::invalid_argument);
  }

  std::vector<deep_todo> output{};
  for (wh::core::json_size_type index = 0; index < todos.value()->Size(); ++index) {
    const auto &item = (*todos.value())[index];
    if (!item.IsObject()) {
      return wh::core::result<std::vector<deep_todo>>::failure(
          wh::core::errc::invalid_argument);
    }

    const auto content = wh::core::json_find_member(item, "content");
    const auto status = wh::core::json_find_member(item, "status");
    if (content.has_error() || status.has_error() || !content.value()->IsString() ||
        !status.value()->IsString()) {
      return wh::core::result<std::vector<deep_todo>>::failure(
          wh::core::errc::invalid_argument);
    }

    deep_todo todo{
        .content =
            std::string{content.value()->GetString(),
                        static_cast<std::size_t>(content.value()->GetStringLength())},
        .status =
            std::string{status.value()->GetString(),
                        static_cast<std::size_t>(status.value()->GetStringLength())},
    };
    if (todo.status != "pending" && todo.status != "in_progress" &&
        todo.status != "completed") {
      return wh::core::result<std::vector<deep_todo>>::failure(
          wh::core::errc::invalid_argument);
    }
    output.push_back(std::move(todo));
  }
  return output;
}

[[nodiscard]] inline auto
make_deep_task_tool_schema(const std::string_view tool_name)
    -> wh::schema::tool_schema_definition {
  wh::schema::tool_schema_definition schema{};
  schema.name = std::string{tool_name};
  schema.description = "dispatch one task to one selected subagent";
  schema.parameters.push_back(wh::schema::tool_parameter_schema{
      .name = "subagent_type",
      .type = wh::schema::tool_parameter_type::string,
      .description = "registered task subagent type",
      .required = true,
  });
  schema.parameters.push_back(wh::schema::tool_parameter_schema{
      .name = "description",
      .type = wh::schema::tool_parameter_type::string,
      .description = "task request for the selected subagent",
      .required = true,
  });
  return schema;
}

[[nodiscard]] inline auto
make_deep_todo_tool_schema(const std::string_view tool_name)
    -> wh::schema::tool_schema_definition {
  wh::schema::tool_schema_definition schema{};
  schema.name = std::string{tool_name};
  schema.description = "write the current task list using pending/in_progress/completed";
  schema.parameters.push_back(wh::schema::tool_parameter_schema{
      .name = "todos",
      .type = wh::schema::tool_parameter_type::array,
      .description = "ordered task list",
      .required = true,
      .item_types =
          {wh::schema::tool_parameter_schema{
              .name = "todo",
              .type = wh::schema::tool_parameter_type::object,
              .description = "one todo item",
              .required = true,
              .properties =
                  {
                      wh::schema::tool_parameter_schema{
                          .name = "content",
                          .type = wh::schema::tool_parameter_type::string,
                          .description = "todo description",
                          .required = true,
                      },
                      wh::schema::tool_parameter_schema{
                          .name = "status",
                          .type = wh::schema::tool_parameter_type::string,
                          .description = "pending/in_progress/completed",
                          .required = true,
                          .enum_values = {"pending", "in_progress", "completed"},
                      },
                  },
          }},
  });
  return schema;
}

} // namespace detail

/// Prebuilt deep-task wrapper that reuses the shared ReAct scenario and injects
/// task tools only when task subagents are available.
template <wh::model::chat_model_like model_t>
class deep {
public:
  /// Creates one deep-task wrapper from the required name, description, and model.
  deep(std::string name, std::string description, model_t model) noexcept
      : name_(std::move(name)), description_(std::move(description)),
        model_(std::move(model)) {}

  deep(const deep &) = delete;
  auto operator=(const deep &) -> deep & = delete;
  deep(deep &&) noexcept = default;
  auto operator=(deep &&) noexcept -> deep & = default;
  ~deep() = default;

  /// Returns the stable scenario name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Replaces the frozen authoring options.
  auto set_options(deep_options options) -> wh::core::result<void> {
    options_ = std::move(options);
    return {};
  }

  /// Registers one single-message task subagent callable.
  auto add_subagent_invoke(std::string type, std::string description,
                           detail::deep_subagent_invoke invoke)
      -> wh::core::result<void> {
    if (type.empty() || description.empty() || !static_cast<bool>(invoke)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    subagents_.push_back(detail::deep_subagent_entry{
        .type = std::move(type),
        .description = std::move(description),
        .invoke = std::move(invoke),
    });
    return {};
  }

  /// Registers one model-backed task subagent.
  template <wh::model::chat_model_like subagent_model_t>
  auto add_subagent_model(std::string type, std::string description,
                          subagent_model_t model) -> wh::core::result<void> {
    return add_subagent_invoke(
        std::move(type), std::move(description),
        [model = std::move(model)](const std::string_view request_text,
                                   wh::core::run_context &context)
            -> wh::flow::agent::message_result {
          wh::model::chat_request request{};
          wh::schema::message message{};
          message.role = wh::schema::message_role::user;
          message.parts.emplace_back(
              wh::schema::text_part{std::string{request_text}});
          request.messages.push_back(std::move(message));
          auto status = model.invoke(request, context);
          if (status.has_error()) {
            return wh::flow::agent::message_result::failure(status.error());
          }
          return std::move(status).value().message;
        });
  }

  /// Registers the state/report types into the supplied serialization registry.
  static auto register_state_types(wh::schema::serialization_registry &registry)
      -> wh::core::result<void> {
    auto todo = registry.register_type<deep_todo>("wh.adk.prebuilt.deep.todo");
    if (todo.has_error() && todo.error() != wh::core::errc::already_exists) {
      return todo;
    }
    auto state = registry.register_type<deep_state>("wh.adk.prebuilt.deep.state");
    if (state.has_error() && state.error() != wh::core::errc::already_exists) {
      return state;
    }
    auto report = registry.register_type<deep_report>("wh.adk.prebuilt.deep.report");
    if (report.has_error() && report.error() != wh::core::errc::already_exists) {
      return report;
    }
    return {};
  }

  /// Runs the deep-task scenario once.
  auto run(const wh::model::chat_request &request,
           wh::core::run_context &context) const -> auto {
    return execute(request, context);
  }

  /// Runs the deep-task scenario once from a movable request payload.
  auto run(wh::model::chat_request &&request, wh::core::run_context &context) const
      -> auto {
    return execute(std::move(request), context);
  }

private:
  struct deep_runtime_state {
    /// Mutable scenario state kept alive until sender completion.
    deep_state state{};
    /// Immutable scenario report accumulated during the run.
    deep_report report{};
    /// Wrapped ReAct agent whose lifetime must cross the async boundary.
    wh::adk::prebuilt::react<model_t> agent;

    deep_runtime_state(std::string name, std::string description, const model_t &model) noexcept
        : agent(std::move(name), std::move(description), model) {}
  };

  [[nodiscard]] static auto
  finish_result(std::unique_ptr<deep_runtime_state> runtime_state,
                chat_model_agent_result result, std::string output_key)
      -> deep_result {
    runtime_state->report.completed = result.report.completed;
    runtime_state->report.final_error = result.report.final_error;

    if (result.report.final_message.has_value()) {
      runtime_state->state.final_text =
          wh::adk::render_message_text(*result.report.final_message);
    } else if (result.report.final_text.has_value()) {
      runtime_state->state.final_text = result.report.final_text;
    }
    if (!output_key.empty() && runtime_state->state.final_text.has_value()) {
      runtime_state->state.output_values.insert_or_assign(
          std::move(output_key), *runtime_state->state.final_text);
    }

    return deep_result{
        .events = std::move(result.events),
        .state = std::move(runtime_state->state),
        .report = std::move(runtime_state->report),
    };
  }

  template <typename sender_t>
  [[nodiscard]] static auto map_deep_sender(std::unique_ptr<deep_runtime_state> runtime_state,
                                            sender_t &&sender,
                                            std::string output_key) {
    using deep_run_result = wh::core::result<deep_result>;
    return wh::core::detail::normalize_result_sender<deep_run_result>(
        wh::core::detail::map_result_sender<deep_run_result>(
            std::forward<sender_t>(sender),
            [runtime_state = std::move(runtime_state),
             output_key = std::move(output_key)](
                chat_model_agent_result result) mutable -> deep_result {
              return finish_result(std::move(runtime_state), std::move(result),
                                   std::move(output_key));
            }));
  }

  [[nodiscard]] auto execute(wh::model::chat_request request,
                             wh::core::run_context &context) const {
    using deep_run_result = wh::core::result<deep_result>;
    using failure_sender_t = decltype(
        wh::core::detail::failure_result_sender<deep_run_result>(
            wh::core::errc::internal_error));
    using run_sender_t = decltype(std::declval<wh::adk::prebuilt::react<model_t> &>().run(
        std::declval<wh::model::chat_request>(), std::declval<wh::core::run_context &>()));
    using mapped_sender_t =
        decltype(map_deep_sender(std::declval<std::unique_ptr<deep_runtime_state>>(),
                                 std::declval<run_sender_t>(), std::string{}));
    using dispatch_sender_t =
        wh::core::detail::variant_sender<failure_sender_t, mapped_sender_t>;

    return wh::core::detail::defer_result_sender<deep_run_result>(
        [this, request = std::move(request), &context]() mutable
            -> dispatch_sender_t {
          if (name_.empty()) {
            return dispatch_sender_t{
                wh::core::detail::failure_result_sender<deep_run_result>(
                    wh::core::errc::invalid_argument)};
          }

          auto runtime_state = std::make_unique<deep_runtime_state>(
              name_, description_, model_);
          runtime_state->report.task_tool_injected = !subagents_.empty();

          auto max_iterations =
              runtime_state->agent.set_max_iterations(options_.max_iterations);
          if (max_iterations.has_error()) {
            return dispatch_sender_t{
                wh::core::detail::failure_result_sender<deep_run_result>(
                    max_iterations.error())};
          }
          auto emit_internal =
              runtime_state->agent.set_emit_internal_events(
                  options_.emit_internal_events);
          if (emit_internal.has_error()) {
            return dispatch_sender_t{
                wh::core::detail::failure_result_sender<deep_run_result>(
                    emit_internal.error())};
          }
          if (!options_.output_key.empty()) {
            auto output_key =
                runtime_state->agent.set_output_key(options_.output_key);
            if (output_key.has_error()) {
              return dispatch_sender_t{
                  wh::core::detail::failure_result_sender<deep_run_result>(
                      output_key.error())};
            }
            auto output_mode = runtime_state->agent.set_output_mode(
                wh::agent::react_output_mode::stream);
            if (output_mode.has_error()) {
              return dispatch_sender_t{
                  wh::core::detail::failure_result_sender<deep_run_result>(
                      output_mode.error())};
            }
          }

          auto *runtime = runtime_state.get();
          if (!subagents_.empty()) {
            auto added_task = runtime_state->agent.add_tool_entry(
                detail::make_deep_task_tool_schema(options_.task_tool_name),
                wh::compose::tool_entry{
                    .invoke =
                        [this, runtime](const wh::compose::tool_call &call,
                                        wh::tool::call_scope scope)
                            -> wh::core::result<wh::compose::graph_value> {
                      auto parsed =
                          detail::parse_deep_task_arguments(call.arguments);
                      if (parsed.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(
                            parsed.error());
                      }

                      for (const auto &subagent : subagents_) {
                        if (subagent.type != parsed.value().first) {
                          continue;
                        }
                        runtime->report.invoked_subagents.push_back(subagent.type);
                        auto message =
                            subagent.invoke(parsed.value().second, scope.run);
                        if (message.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(
                              message.error());
                        }
                        return wh::compose::graph_value{
                            wh::flow::agent::render_message_text(
                                std::move(message).value())};
                      }

                      return wh::core::result<wh::compose::graph_value>::failure(
                          wh::core::errc::invalid_argument);
                    }});
            if (added_task.has_error()) {
              return dispatch_sender_t{
                  wh::core::detail::failure_result_sender<deep_run_result>(
                      added_task.error())};
            }

            if (options_.enable_write_todos) {
              auto added_todos = runtime_state->agent.add_tool_entry(
                  detail::make_deep_todo_tool_schema(options_.todo_tool_name),
                  wh::compose::tool_entry{
                      .invoke =
                          [runtime](const wh::compose::tool_call &call,
                                    wh::tool::call_scope)
                              -> wh::core::result<wh::compose::graph_value> {
                        auto todos = detail::parse_deep_todos(call.arguments);
                        if (todos.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(
                              todos.error());
                        }
                        runtime->state.todos = std::move(todos).value();
                        return wh::compose::graph_value{
                            std::string{"todos updated"}};
                      }});
              if (added_todos.has_error()) {
                return dispatch_sender_t{
                    wh::core::detail::failure_result_sender<deep_run_result>(
                        added_todos.error())};
              }
            }
          }

          auto agent_sender = runtime_state->agent.run(std::move(request), context);
          return dispatch_sender_t{map_deep_sender(std::move(runtime_state),
                                                   std::move(agent_sender),
                                                   options_.output_key)};
        });
  }

  /// Stable scenario name.
  std::string name_{};
  /// Human-readable scenario description.
  std::string description_{};
  /// Shared model reused by the wrapped ReAct scenario.
  model_t model_;
  /// Frozen authoring options.
  deep_options options_{};
  /// Registered task subagents in stable insertion order.
  std::vector<detail::deep_subagent_entry> subagents_{};
};

} // namespace wh::adk::prebuilt
