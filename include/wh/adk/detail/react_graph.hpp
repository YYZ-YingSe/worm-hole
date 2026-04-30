// Defines the internal ReAct graph lowerer that maps the public authored shell
// onto one compose graph without introducing a second runtime.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/adk/detail/agent_graph_view.hpp"
#include "wh/adk/detail/history_request.hpp"
#include "wh/adk/detail/shared_state.hpp"
#include "wh/adk/deterministic_transfer.hpp"
#include "wh/agent/agent.hpp"
#include "wh/agent/react.hpp"
#include "wh/compose/authored/value_branch.hpp"
#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/tools_builder.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/model/chat_model.hpp"

namespace wh::adk::detail {

namespace react_detail {

struct prepared_tool_round {
  wh::schema::message assistant_message{};
  std::vector<wh::agent::react_tool_action> actions{};
};

struct model_output_snapshot {
  std::vector<wh::schema::message> messages{};
  wh::schema::message assistant_message{};
  bool has_tool_call{false};
  std::vector<wh::agent::react_tool_action> actions{};
};

struct apply_tools_decision {
  bool direct_return{false};
  std::optional<wh::schema::message> direct_message{};
  std::vector<wh::schema::message> tool_messages{};
};

[[nodiscard]] inline auto read_react_state(wh::compose::graph_process_state &process_state)
    -> wh::core::result<std::reference_wrapper<wh::agent::react_state>> {
  return wh::adk::detail::shared_state_ref<wh::agent::react_state>(process_state);
}

[[nodiscard]] inline auto make_instruction_message(const std::string_view description,
                                                   const std::string_view instruction)
    -> std::optional<wh::schema::message> {
  std::string text{};
  if (!description.empty()) {
    text.append(description);
  }
  if (!instruction.empty()) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    text.append(instruction);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  wh::schema::message message{};
  message.role = wh::schema::message_role::system;
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

[[nodiscard]] inline auto render_message_text(const wh::schema::message &message) -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part); typed != nullptr) {
      text.append(typed->text);
    }
  }
  return text;
}

[[nodiscard]] inline auto collect_tool_actions(const wh::schema::message &message,
                                               const wh::agent::toolset &tools)
    -> std::vector<wh::agent::react_tool_action> {
  std::vector<wh::agent::react_tool_action> actions{};
  for (const auto &part : message.parts) {
    const auto *tool = std::get_if<wh::schema::tool_call_part>(&part);
    if (tool == nullptr) {
      continue;
    }
    actions.push_back(wh::agent::react_tool_action{
        .call_id = tool->id,
        .tool_name = tool->name,
        .arguments = tool->arguments,
        .return_direct = tools.is_return_direct_tool(tool->name),
    });
  }
  return actions;
}

[[nodiscard]] inline auto
make_tool_batch(const std::span<const wh::agent::react_tool_action> actions)
    -> wh::compose::tool_batch {
  wh::compose::tool_batch batch{};
  batch.calls.reserve(actions.size());
  for (const auto &action : actions) {
    batch.calls.push_back(wh::compose::tool_call{
        .call_id = action.call_id,
        .tool_name = action.tool_name,
        .arguments = action.arguments,
    });
  }
  return batch;
}

[[nodiscard]] inline auto
make_tool_batch(const std::span<const wh::agent::react_tool_action> actions,
                const wh::model::chat_request &history_request) -> wh::compose::tool_batch {
  auto batch = make_tool_batch(actions);
  for (auto &call : batch.calls) {
    call.payload = wh::core::any(wh::adk::detail::history_request_payload{
        .history_request = history_request,
    });
  }
  return batch;
}

[[nodiscard]] inline auto tool_result_to_message(const wh::compose::tool_result &result)
    -> wh::core::result<wh::schema::message> {
  wh::schema::message message{};
  message.role = wh::schema::message_role::tool;
  message.tool_call_id = result.call_id;
  message.tool_name = result.tool_name;

  if (const auto *text = wh::core::any_cast<std::string>(&result.value); text != nullptr) {
    message.parts.emplace_back(wh::schema::text_part{*text});
    return message;
  }
  if (const auto *typed = wh::core::any_cast<wh::schema::message>(&result.value);
      typed != nullptr) {
    message = *typed;
    if (message.tool_call_id.empty()) {
      message.tool_call_id = result.call_id;
    }
    if (message.tool_name.empty()) {
      message.tool_name = result.tool_name;
    }
    if (message.role != wh::schema::message_role::tool) {
      message.role = wh::schema::message_role::tool;
    }
    return message;
  }
  return wh::core::result<wh::schema::message>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto tool_event_to_message(const wh::compose::tool_event &event)
    -> wh::core::result<wh::schema::message> {
  wh::schema::message message{};
  message.role = wh::schema::message_role::tool;
  message.tool_call_id = event.call_id;
  message.tool_name = event.tool_name;

  if (const auto *text = wh::core::any_cast<std::string>(&event.value); text != nullptr) {
    message.parts.emplace_back(wh::schema::text_part{*text});
    return message;
  }
  if (const auto *typed = wh::core::any_cast<wh::schema::message>(&event.value); typed != nullptr) {
    message = *typed;
    if (message.tool_call_id.empty()) {
      message.tool_call_id = event.call_id;
    }
    if (message.tool_name.empty()) {
      message.tool_name = event.tool_name;
    }
    if (message.role != wh::schema::message_role::tool) {
      message.role = wh::schema::message_role::tool;
    }
    return message;
  }
  return wh::core::result<wh::schema::message>::failure(wh::core::errc::type_mismatch);
}

inline auto write_output_value(wh::agent::agent_output &output, const std::string_view output_key,
                               const wh::agent::react_output_mode output_mode,
                               const wh::schema::message &message) -> void {
  if (output_key.empty()) {
    return;
  }
  if (output_mode == wh::agent::react_output_mode::value) {
    output.output_values.insert_or_assign(std::string{output_key}, wh::core::any{message});
    return;
  }
  output.output_values.insert_or_assign(std::string{output_key},
                                        wh::core::any{render_message_text(message)});
}

[[nodiscard]] inline auto make_agent_output(std::vector<wh::schema::message> messages,
                                            const std::string_view output_key,
                                            const wh::agent::react_output_mode output_mode)
    -> wh::core::result<wh::agent::agent_output> {
  if (messages.empty()) {
    return wh::core::result<wh::agent::agent_output>::failure(wh::core::errc::not_found);
  }
  auto final_message = messages.back();
  wh::agent::agent_output output{
      .final_message = final_message,
      .history_messages = std::move(messages),
      .transfer = wh::adk::extract_transfer_from_message(final_message),
  };
  write_output_value(output, output_key, output_mode, output.final_message);
  return output;
}

[[nodiscard]] inline auto streamify_invoke_result(wh::core::result<wh::compose::graph_value> status)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  if (status.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(status.error());
  }
  return wh::compose::make_single_value_stream_reader(std::move(status).value());
}

[[nodiscard]] inline auto
normalize_tool_entry_for_stream(const wh::compose::tool_entry &entry,
                                const wh::compose::node_exec_mode exec_mode)
    -> wh::core::result<wh::compose::tool_entry> {
  auto normalized = entry;
  if (exec_mode == wh::compose::node_exec_mode::sync) {
    if (static_cast<bool>(normalized.stream)) {
      return normalized;
    }
    if (!static_cast<bool>(normalized.invoke)) {
      return wh::core::result<wh::compose::tool_entry>::failure(wh::core::errc::not_supported);
    }
    normalized.stream = wh::compose::tool_stream{
        [invoke = normalized.invoke](const wh::compose::tool_call &call, wh::tool::call_scope scope)
            -> wh::core::result<wh::compose::graph_stream_reader> {
          return streamify_invoke_result(invoke(call, std::move(scope)));
        }};
    return normalized;
  }

  if (static_cast<bool>(normalized.async_stream)) {
    return normalized;
  }
  if (!static_cast<bool>(normalized.async_invoke)) {
    return wh::core::result<wh::compose::tool_entry>::failure(wh::core::errc::not_supported);
  }
  normalized.async_stream = wh::compose::tool_async_stream{
      [async_invoke = normalized.async_invoke](
          wh::compose::tool_call call,
          wh::tool::call_scope scope) -> wh::compose::tools_stream_sender {
        auto sender = async_invoke(std::move(call), std::move(scope)) |
                      stdexec::then([](wh::core::result<wh::compose::graph_value> status)
                                        -> wh::core::result<wh::compose::graph_stream_reader> {
                        return streamify_invoke_result(std::move(status));
                      });
        return wh::compose::tools_stream_sender{std::move(sender)};
      }};
  return normalized;
}

[[nodiscard]] inline auto
normalize_tools_registry_for_stream(const wh::compose::tool_registry &registry,
                                    const wh::compose::node_exec_mode exec_mode)
    -> wh::core::result<wh::compose::tool_registry> {
  wh::compose::tool_registry normalized{};
  normalized.reserve(registry.size());
  for (const auto &[name, entry] : registry) {
    auto normalized_entry = normalize_tool_entry_for_stream(entry, exec_mode);
    if (normalized_entry.has_error()) {
      return wh::core::result<wh::compose::tool_registry>::failure(normalized_entry.error());
    }
    normalized.emplace(name, std::move(normalized_entry).value());
  }
  return normalized;
}

[[nodiscard]] inline auto read_tool_event_values(std::vector<wh::compose::graph_value> values,
                                                 const wh::agent::toolset &toolset)
    -> wh::core::result<apply_tools_decision> {
  apply_tools_decision decision{};
  decision.tool_messages.reserve(values.size());
  for (auto &value : values) {
    auto *event = wh::core::any_cast<wh::compose::tool_event>(&value);
    if (event == nullptr) {
      return wh::core::result<apply_tools_decision>::failure(wh::core::errc::type_mismatch);
    }
    auto message = tool_event_to_message(*event);
    if (message.has_error()) {
      return wh::core::result<apply_tools_decision>::failure(message.error());
    }
    if (!decision.direct_return && toolset.is_return_direct_tool(event->tool_name)) {
      decision.direct_return = true;
      decision.direct_message = message.value();
    }
    decision.tool_messages.push_back(std::move(message).value());
  }
  return decision;
}

[[nodiscard]] inline auto read_tool_event_payload(wh::compose::graph_value &payload,
                                                  const wh::agent::toolset &toolset)
    -> wh::core::result<apply_tools_decision> {
  if (auto *values = wh::core::any_cast<std::vector<wh::compose::graph_value>>(&payload);
      values != nullptr) {
    return read_tool_event_values(std::move(*values), toolset);
  }
  if (auto *event = wh::core::any_cast<wh::compose::tool_event>(&payload); event != nullptr) {
    std::vector<wh::compose::graph_value> values{};
    values.emplace_back(std::move(*event));
    return read_tool_event_values(std::move(values), toolset);
  }
  return wh::core::result<apply_tools_decision>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto make_model_output_snapshot(std::vector<wh::schema::message> messages,
                                                     const wh::agent::toolset &toolset)
    -> wh::core::result<model_output_snapshot> {
  if (messages.empty()) {
    return wh::core::result<model_output_snapshot>::failure(wh::core::errc::not_found);
  }
  auto assistant_message = messages.back();
  auto actions = collect_tool_actions(assistant_message, toolset);
  return model_output_snapshot{
      .messages = std::move(messages),
      .assistant_message = std::move(assistant_message),
      .has_tool_call = !actions.empty(),
      .actions = std::move(actions),
  };
}

[[nodiscard]] inline auto read_model_output_snapshot(wh::compose::graph_value &payload,
                                                     const wh::agent::toolset &toolset)
    -> wh::core::result<model_output_snapshot> {
  if (auto *snapshot = wh::core::any_cast<model_output_snapshot>(&payload); snapshot != nullptr) {
    return std::move(*snapshot);
  }
  auto messages = wh::adk::detail::read_message_payload(payload);
  if (messages.has_error()) {
    return wh::core::result<model_output_snapshot>::failure(messages.error());
  }
  return make_model_output_snapshot(std::move(messages).value(), toolset);
}

[[nodiscard]] inline auto make_bootstrap_options(const std::size_t max_iterations)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre([max_iterations](const wh::compose::graph_state_cause &,
                                          wh::compose::graph_process_state &process_state,
                                          wh::compose::graph_value &payload,
                                          wh::core::run_context &) -> wh::core::result<void> {
    auto *messages = wh::core::any_cast<std::vector<wh::schema::message>>(&payload);
    if (messages == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    auto initialized = wh::adk::detail::emplace_shared_state<wh::agent::react_state>(
        process_state, wh::agent::react_state{
                           .messages = std::move(*messages),
                           .remaining_iterations = max_iterations,
                       });
    if (initialized.has_error()) {
      return wh::core::result<void>::failure(initialized.error());
    }
    payload = wh::core::any(std::monostate{});
    return {};
  });
  return options;
}

[[nodiscard]] inline auto
make_prepare_request_options(std::string description, std::string instruction,
                             std::vector<wh::schema::tool_schema_definition> tools)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre(
      [description = std::move(description), instruction = std::move(instruction),
       tools = std::move(tools)](
          const wh::compose::graph_state_cause &, wh::compose::graph_process_state &process_state,
          wh::compose::graph_value &payload, wh::core::run_context &) -> wh::core::result<void> {
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        if (react_state.remaining_iterations == 0U) {
          return wh::core::result<void>::failure(wh::core::errc::resource_exhausted);
        }

        wh::model::chat_request request{};
        request.messages.reserve(react_state.messages.size() + 1U);
        if (auto system = make_instruction_message(description, instruction); system.has_value()) {
          request.messages.push_back(std::move(*system));
        }
        for (const auto &message : react_state.messages) {
          request.messages.push_back(message);
        }
        request.tools = tools;
        payload = wh::core::any(std::move(request));
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_prepare_tools_options(wh::agent::toolset tools)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [tools = std::move(tools)](
          const wh::compose::graph_state_cause &, wh::compose::graph_process_state &process_state,
          wh::compose::graph_value &payload, wh::core::run_context &) -> wh::core::result<void> {
        auto *prepared = wh::core::any_cast<prepared_tool_round>(&payload);
        if (prepared == nullptr) {
          return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
        }
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        if (react_state.remaining_iterations == 0U) {
          return wh::core::result<void>::failure(wh::core::errc::resource_exhausted);
        }

        react_state.remaining_iterations -= 1U;
        react_state.messages.push_back(prepared->assistant_message);
        react_state.pending_tool_actions = prepared->actions;
        react_state.return_direct_call_id.reset();

        auto history_request = wh::adk::detail::make_history_request(react_state);
        if (history_request.has_value()) {
          payload = wh::core::any(make_tool_batch(prepared->actions, history_request.value()));
        } else {
          payload = wh::core::any(make_tool_batch(prepared->actions));
        }
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_apply_tools_options() -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload, wh::core::run_context &) -> wh::core::result<void> {
        auto *decision = wh::core::any_cast<apply_tools_decision>(&payload);
        if (decision == nullptr) {
          return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
        }
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        for (const auto &message : decision->tool_messages) {
          react_state.messages.push_back(message);
        }
        react_state.pending_tool_actions.clear();
        if (decision->direct_return && decision->direct_message.has_value()) {
          react_state.return_direct_call_id = decision->direct_message->tool_call_id;
        } else {
          react_state.return_direct_call_id.reset();
        }
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_finalize_history_options() -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload, wh::core::run_context &) -> wh::core::result<void> {
        auto *message = wh::core::any_cast<wh::schema::message>(&payload);
        if (message == nullptr) {
          return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
        }
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        react_state.messages.push_back(*message);
        payload = wh::core::any(react_state.messages);
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_emit_direct_history_options()
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload, wh::core::run_context &) -> wh::core::result<void> {
        auto *message = wh::core::any_cast<wh::schema::message>(&payload);
        if (message == nullptr) {
          return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
        }
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        react_state.messages.push_back(*message);
        payload = wh::core::any(react_state.messages);
        return {};
      });
  return options;
}

} // namespace react_detail

/// Internal ReAct lowering shell that produces one real compose graph.
class react_graph {
public:
  explicit react_graph(const wh::agent::react &authored) noexcept
      : authored_(std::addressof(authored)) {}

  [[nodiscard]] auto lower() const -> wh::core::result<wh::compose::graph> {
    return lower_native();
  }

private:
  [[nodiscard]] auto lower_native() const -> wh::core::result<wh::compose::graph> {
    if (authored_ == nullptr) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::invalid_argument);
    }

    auto model_binding = authored_->model_binding();
    if (model_binding.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_binding.error());
    }
    auto model_node =
        model_binding.value().get().materialize(std::string{wh::agent::react_model_node_key});
    if (model_node.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_node.error());
    }
    const auto tools_options = authored_->tools().node_options();
    if (!tools_options.has_value()) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::contract_violation);
    }

    auto toolset = authored_->tools();
    auto request_transforms = std::vector<wh::agent::middlewares::request_transform_binding>{
        authored_->request_transforms().begin(), authored_->request_transforms().end()};
    auto registry = react_detail::normalize_tools_registry_for_stream(toolset.registry(),
                                                                      tools_options->exec_mode);
    if (registry.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(registry.error());
    }
    auto normalized_registry = std::move(registry).value();
    auto runtime_options = toolset.runtime_options();
    if (runtime_options.missing.has_value()) {
      auto normalized_missing = react_detail::normalize_tool_entry_for_stream(
          *runtime_options.missing, tools_options->exec_mode);
      if (normalized_missing.has_error()) {
        return wh::core::result<wh::compose::graph>::failure(normalized_missing.error());
      }
      runtime_options.missing = std::move(normalized_missing).value();
    }
    auto description = std::string{authored_->description()};
    auto instruction = authored_->render_instruction();
    const auto max_iterations = authored_->max_iterations();

    wh::compose::graph_compile_options compile_options{};
    compile_options.name = std::string{authored_->name()};
    compile_options.boundary.output = wh::compose::node_contract::stream;
    compile_options.mode = wh::compose::graph_runtime_mode::pregel;
    compile_options.max_steps = max_iterations * 4U + 4U;
    compile_options.max_parallel_nodes = 1U;
    compile_options.max_parallel_per_node = 1U;
    wh::compose::graph lowered{std::move(compile_options)};

    auto bootstrap = wh::compose::make_lambda_node(
        "bootstrap",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        react_detail::make_bootstrap_options(max_iterations));
    auto bootstrap_added = lowered.add_lambda(std::move(bootstrap));
    if (bootstrap_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(bootstrap_added.error());
    }

    auto prepare_request = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                         wh::compose::node_contract::value,
                                                         wh::compose::node_exec_mode::async>(
        "prepare_request",
        [request_transforms = std::move(request_transforms)](
            wh::compose::graph_value &input, wh::core::run_context &context,
            const wh::compose::graph_call_scope &) -> wh::compose::graph_value_sender {
          auto *request = wh::core::any_cast<wh::model::chat_request>(&input);
          if (request == nullptr) {
            return wh::compose::graph_value_sender{
                wh::core::detail::failure_result_sender<wh::core::result<wh::compose::graph_value>>(
                    wh::core::errc::type_mismatch)};
          }
          return wh::compose::graph_value_sender{
              wh::core::detail::map_result_sender<wh::core::result<wh::compose::graph_value>>(
                  wh::agent::middlewares::apply_request_transforms(request_transforms,
                                                                   std::move(*request), context),
                  [](wh::model::chat_request transformed) -> wh::compose::graph_value {
                    return wh::core::any(std::move(transformed));
                  })};
        },
        react_detail::make_prepare_request_options(
            std::move(description), std::move(instruction),
            std::vector<wh::schema::tool_schema_definition>{toolset.schemas().begin(),
                                                            toolset.schemas().end()}));
    auto prepare_request_added = lowered.add_lambda(std::move(prepare_request));
    if (prepare_request_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(prepare_request_added.error());
    }

    auto model_added = lowered.add_component(std::move(model_node).value());
    if (model_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_added.error());
    }

    auto model_snapshot = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                        wh::compose::node_contract::value>(
        "model_snapshot",
        [toolset](
            wh::compose::graph_value &input, wh::core::run_context &,
            const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto snapshot = react_detail::read_model_output_snapshot(input, toolset);
          if (snapshot.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(snapshot.error());
          }
          return wh::core::any(std::move(snapshot).value());
        });
    auto model_snapshot_added = lowered.add_lambda(std::move(model_snapshot));
    if (model_snapshot_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_snapshot_added.error());
    }

    auto prepare_tools = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                       wh::compose::node_contract::value>(
        "prepare_tools",
        [toolset](
            wh::compose::graph_value &input, wh::core::run_context &,
            const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto snapshot = react_detail::read_model_output_snapshot(input, toolset);
          if (snapshot.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(snapshot.error());
          }
          auto prepared = std::move(snapshot).value();
          return wh::core::any(react_detail::prepared_tool_round{
              .assistant_message = std::move(prepared.assistant_message),
              .actions = std::move(prepared.actions),
          });
        },
        react_detail::make_prepare_tools_options(toolset));
    auto prepare_tools_added = lowered.add_lambda(std::move(prepare_tools));
    if (prepare_tools_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(prepare_tools_added.error());
    }

    wh::core::result<void> tools_added{};
    if (tools_options->exec_mode == wh::compose::node_exec_mode::sync) {
      tools_added =
          lowered.add_tools(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                         wh::compose::node_contract::stream,
                                                         wh::compose::node_exec_mode::sync>(
              "tools", std::move(normalized_registry), {}, std::move(runtime_options)));
    } else {
      tools_added =
          lowered.add_tools(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                         wh::compose::node_contract::stream,
                                                         wh::compose::node_exec_mode::async>(
              "tools", std::move(normalized_registry), {}, std::move(runtime_options)));
    }
    if (tools_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(tools_added.error());
    }

    auto apply_tools = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value>(
        "apply_tools",
        [toolset](
            wh::compose::graph_value &input, wh::core::run_context &,
            const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto decision = react_detail::read_tool_event_payload(input, toolset);
          if (decision.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(decision.error());
          }
          return wh::core::any(std::move(decision).value());
        },
        react_detail::make_apply_tools_options());
    auto apply_tools_added = lowered.add_lambda(std::move(apply_tools));
    if (apply_tools_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(apply_tools_added.error());
    }

    auto emit_direct = wh::compose::make_lambda_node(
        "emit_direct_history",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto *decision = wh::core::any_cast<react_detail::apply_tools_decision>(&input);
          if (decision == nullptr || !decision->direct_message.has_value()) {
            return wh::core::result<wh::compose::graph_value>::failure(wh::core::errc::not_found);
          }
          return wh::core::any(*decision->direct_message);
        },
        react_detail::make_emit_direct_history_options());
    auto emit_direct_added = lowered.add_lambda(std::move(emit_direct));
    if (emit_direct_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(emit_direct_added.error());
    }

    auto finalize = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                  wh::compose::node_contract::value>(
        "finalize_history",
        [toolset](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto snapshot = react_detail::read_model_output_snapshot(input, toolset);
          if (snapshot.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(snapshot.error());
          }
          return wh::core::any(std::move(snapshot).value().assistant_message);
        },
        react_detail::make_finalize_history_options());
    auto finalize_added = lowered.add_lambda(std::move(finalize));
    if (finalize_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finalize_added.error());
    }

    auto emit_history = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                      wh::compose::node_contract::stream>(
        "emit_history",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_stream_reader> {
          auto *messages = wh::core::any_cast<std::vector<wh::schema::message>>(&input);
          if (messages == nullptr) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(
                wh::core::errc::type_mismatch);
          }
          return wh::adk::detail::make_message_stream_reader(std::move(*messages));
        });
    auto emit_history_added = lowered.add_lambda(std::move(emit_history));
    if (emit_history_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(emit_history_added.error());
    }

    auto start_added = lowered.add_entry_edge("bootstrap");
    if (start_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(start_added.error());
    }
    auto bootstrap_edge = lowered.add_edge("bootstrap", "prepare_request");
    if (bootstrap_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(bootstrap_edge.error());
    }
    auto request_edge =
        lowered.add_edge("prepare_request", std::string{wh::agent::react_model_node_key});
    if (request_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(request_edge.error());
    }
    auto model_snapshot_edge =
        lowered.add_edge(std::string{wh::agent::react_model_node_key}, "model_snapshot");
    if (model_snapshot_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_snapshot_edge.error());
    }
    auto prepare_tools_edge = lowered.add_edge("prepare_tools", "tools");
    if (prepare_tools_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(prepare_tools_edge.error());
    }
    auto apply_tools_edge = lowered.add_edge("tools", "apply_tools");
    if (apply_tools_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(apply_tools_edge.error());
    }
    auto emit_direct_edge = lowered.add_edge("emit_direct_history", "emit_history");
    if (emit_direct_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(emit_direct_edge.error());
    }
    auto finalize_edge = lowered.add_edge("finalize_history", "emit_history");
    if (finalize_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finalize_edge.error());
    }
    auto history_exit = lowered.add_exit_edge("emit_history");
    if (history_exit.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(history_exit.error());
    }

    wh::compose::value_branch model_branch{};
    auto add_prepare_case = model_branch.add_case(
        "prepare_tools",
        [](const wh::compose::graph_value &output,
           wh::core::run_context &) -> wh::core::result<bool> {
          const auto *snapshot = wh::core::any_cast<react_detail::model_output_snapshot>(&output);
          if (snapshot == nullptr) {
            return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
          }
          return snapshot->has_tool_call;
        });
    if (add_prepare_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(add_prepare_case.error());
    }
    auto add_finalize_case = model_branch.add_case(
        "finalize_history",
        [](const wh::compose::graph_value &output,
           wh::core::run_context &) -> wh::core::result<bool> {
          const auto *snapshot = wh::core::any_cast<react_detail::model_output_snapshot>(&output);
          if (snapshot == nullptr) {
            return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
          }
          return !snapshot->has_tool_call;
        });
    if (add_finalize_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(add_finalize_case.error());
    }
    auto model_branch_added = model_branch.apply(lowered, "model_snapshot");
    if (model_branch_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_branch_added.error());
    }

    wh::compose::value_branch apply_branch{};
    auto add_loop_case = apply_branch.add_case(
        "prepare_request",
        [](const wh::compose::graph_value &output,
           wh::core::run_context &) -> wh::core::result<bool> {
          const auto *decision = wh::core::any_cast<react_detail::apply_tools_decision>(&output);
          if (decision == nullptr) {
            return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
          }
          return !decision->direct_return;
        });
    if (add_loop_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(add_loop_case.error());
    }
    auto add_direct_case = apply_branch.add_case(
        "emit_direct_history",
        [](const wh::compose::graph_value &output,
           wh::core::run_context &) -> wh::core::result<bool> {
          const auto *decision = wh::core::any_cast<react_detail::apply_tools_decision>(&output);
          if (decision == nullptr) {
            return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
          }
          return decision->direct_return;
        });
    if (add_direct_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(add_direct_case.error());
    }
    auto apply_branch_added = apply_branch.apply(lowered, "apply_tools");
    if (apply_branch_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(apply_branch_added.error());
    }

    return lowered;
  }
  const wh::agent::react *authored_{nullptr};
};

/// Wraps one frozen ReAct shell as the common authored-agent surface.
[[nodiscard]] inline auto bind_react_agent(wh::agent::react authored)
    -> wh::core::result<wh::agent::agent> {
  if (!authored.frozen()) {
    return wh::core::result<wh::agent::agent>::failure(wh::core::errc::contract_violation);
  }

  wh::agent::agent exported{std::string{authored.name()}};
  auto described = exported.set_description(std::string{authored.description()});
  if (described.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(described.error());
  }

  auto shell = std::make_unique<wh::agent::react>(std::move(authored));
  auto bound = exported.bind_execution(
      nullptr, [shell = std::move(shell)]() mutable -> wh::core::result<wh::compose::graph> {
        return react_graph{*shell}.lower();
      });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  auto exported_frozen = exported.freeze();
  if (exported_frozen.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(exported_frozen.error());
  }
  return exported;
}

} // namespace wh::adk::detail
