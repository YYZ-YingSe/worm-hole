// Defines the internal ReAct graph lowerer that maps the public authored shell
// onto one compose graph without introducing a second runtime.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "wh/adk/deterministic_transfer.hpp"
#include "wh/adk/detail/history_request.hpp"
#include "wh/adk/detail/shared_state.hpp"
#include "wh/agent/agent.hpp"
#include "wh/agent/react.hpp"
#include "wh/compose/authored/value_branch.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/tools_builder.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/message.hpp"

namespace wh::adk::detail {

namespace react_detail {

struct prepared_tool_round {
  wh::schema::message assistant_message{};
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

[[nodiscard]] inline auto read_model_messages(
    wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<wh::schema::message>> {
  auto values = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (values.has_error()) {
    return wh::core::result<std::vector<wh::schema::message>>::failure(
        values.error());
  }

  std::vector<wh::schema::message> messages{};
  messages.reserve(values.value().size());
  for (auto &value : values.value()) {
    auto *typed = wh::core::any_cast<wh::schema::message>(&value);
    if (typed == nullptr) {
      return wh::core::result<std::vector<wh::schema::message>>::failure(
          wh::core::errc::type_mismatch);
    }
    messages.push_back(std::move(*typed));
  }
  return messages;
}

[[nodiscard]] inline auto make_instruction_message(
    const std::string_view description, const std::string_view instruction)
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

[[nodiscard]] inline auto render_message_text(const wh::schema::message &message)
    -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part);
        typed != nullptr) {
      text.append(typed->text);
    }
  }
  return text;
}

[[nodiscard]] inline auto collect_tool_actions(
    const wh::schema::message &message, const wh::agent::toolset &tools)
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

[[nodiscard]] inline auto make_tool_batch(
    const std::span<const wh::agent::react_tool_action> actions)
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

[[nodiscard]] inline auto make_tool_batch(
    const std::span<const wh::agent::react_tool_action> actions,
    const wh::model::chat_request &history_request)
    -> wh::compose::tool_batch {
  auto batch = make_tool_batch(actions);
  for (auto &call : batch.calls) {
    call.payload = wh::core::any(wh::adk::detail::history_request_payload{
        .history_request = history_request,
    });
  }
  return batch;
}

[[nodiscard]] inline auto tool_result_to_message(
    const wh::compose::tool_result &result)
    -> wh::core::result<wh::schema::message> {
  wh::schema::message message{};
  message.role = wh::schema::message_role::tool;
  message.tool_call_id = result.call_id;
  message.tool_name = result.tool_name;

  if (const auto *text = wh::core::any_cast<std::string>(&result.value);
      text != nullptr) {
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
  return wh::core::result<wh::schema::message>::failure(
      wh::core::errc::type_mismatch);
}

inline auto write_output_value(wh::agent::react_state &state,
                               const std::string_view output_key,
                               const wh::agent::react_output_mode output_mode,
                               const wh::schema::message &message) -> void {
  if (output_key.empty()) {
    return;
  }
  if (output_mode == wh::agent::react_output_mode::value) {
    state.output_values.insert_or_assign(std::string{output_key},
                                         wh::core::any{message});
    return;
  }
  state.output_values.insert_or_assign(std::string{output_key},
                                       wh::core::any{render_message_text(message)});
}

[[nodiscard]] inline auto make_agent_output(const wh::agent::react_state &state,
                                            const wh::schema::message &message)
    -> wh::agent::agent_output {
  return wh::agent::agent_output{
      .final_message = message,
      .history_messages = state.messages,
      .transfer = wh::adk::extract_transfer_from_message(message),
      .output_values = state.output_values,
  };
}

[[nodiscard]] inline auto
make_bootstrap_options(const std::size_t max_iterations)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre(
      [max_iterations](const wh::compose::graph_state_cause &,
                       wh::compose::graph_process_state &process_state,
                       wh::compose::graph_value &payload,
                       wh::core::run_context &) -> wh::core::result<void> {
        auto *messages =
            wh::core::any_cast<std::vector<wh::schema::message>>(&payload);
        if (messages == nullptr) {
          return wh::core::result<void>::failure(
              wh::core::errc::type_mismatch);
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

[[nodiscard]] inline auto make_prepare_request_options(
    std::string description, std::string instruction,
    std::vector<wh::schema::tool_schema_definition> tools)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre(
      [description = std::move(description), instruction = std::move(instruction),
       tools = std::move(tools)](const wh::compose::graph_state_cause &,
                                 wh::compose::graph_process_state &process_state,
                                 wh::compose::graph_value &payload,
                                 wh::core::run_context &)
          -> wh::core::result<void> {
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        if (react_state.remaining_iterations == 0U) {
          return wh::core::result<void>::failure(
              wh::core::errc::resource_exhausted);
        }

        wh::model::chat_request request{};
        request.messages.reserve(react_state.messages.size() + 1U);
        if (auto system = make_instruction_message(description, instruction);
            system.has_value()) {
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
      [tools = std::move(tools)](const wh::compose::graph_state_cause &,
                                 wh::compose::graph_process_state &process_state,
                                 wh::compose::graph_value &payload,
                                 wh::core::run_context &)
          -> wh::core::result<void> {
        auto *prepared = wh::core::any_cast<prepared_tool_round>(&payload);
        if (prepared == nullptr) {
          return wh::core::result<void>::failure(
              wh::core::errc::type_mismatch);
        }
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        if (react_state.remaining_iterations == 0U) {
          return wh::core::result<void>::failure(
              wh::core::errc::resource_exhausted);
        }

        react_state.remaining_iterations -= 1U;
        react_state.messages.push_back(prepared->assistant_message);
        react_state.pending_tool_actions = prepared->actions;
        react_state.return_direct_call_id.reset();

        auto history_request = wh::adk::detail::make_history_request(react_state);
        if (history_request.has_value()) {
          payload = wh::core::any(
              make_tool_batch(prepared->actions, history_request.value()));
        } else {
          payload = wh::core::any(make_tool_batch(prepared->actions));
        }
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_apply_tools_options()
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [](const wh::compose::graph_state_cause &,
         wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload,
         wh::core::run_context &) -> wh::core::result<void> {
        auto *decision = wh::core::any_cast<apply_tools_decision>(&payload);
        if (decision == nullptr) {
          return wh::core::result<void>::failure(
              wh::core::errc::type_mismatch);
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

[[nodiscard]] inline auto make_finalize_options(
    std::string output_key, const wh::agent::react_output_mode output_mode)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [output_key = std::move(output_key), output_mode](
          const wh::compose::graph_state_cause &,
          wh::compose::graph_process_state &process_state,
          wh::compose::graph_value &payload,
          wh::core::run_context &) -> wh::core::result<void> {
        auto *message = wh::core::any_cast<wh::schema::message>(&payload);
        if (message == nullptr) {
          return wh::core::result<void>::failure(
              wh::core::errc::type_mismatch);
        }
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        if (react_state.remaining_iterations == 0U) {
          return wh::core::result<void>::failure(
              wh::core::errc::resource_exhausted);
        }
        react_state.remaining_iterations -= 1U;
        react_state.messages.push_back(*message);
        write_output_value(react_state, output_key, output_mode, *message);
        payload = wh::core::any(
            make_agent_output(react_state, *message));
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_emit_direct_options(
    std::string output_key, const wh::agent::react_output_mode output_mode)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [output_key = std::move(output_key), output_mode](
          const wh::compose::graph_state_cause &,
          wh::compose::graph_process_state &process_state,
          wh::compose::graph_value &payload,
          wh::core::run_context &) -> wh::core::result<void> {
        auto *message = wh::core::any_cast<wh::schema::message>(&payload);
        if (message == nullptr) {
          return wh::core::result<void>::failure(
              wh::core::errc::type_mismatch);
        }
        auto state = read_react_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &react_state = state.value().get();
        write_output_value(react_state, output_key, output_mode, *message);
        payload = wh::core::any(
            make_agent_output(react_state, *message));
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
    if (authored_ == nullptr) {
      return wh::core::result<wh::compose::graph>::failure(
          wh::core::errc::invalid_argument);
    }

    auto model_node = authored_->model_node();
    if (model_node.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_node.error());
    }
    const auto tools_options = authored_->tools().node_options();
    if (!tools_options.has_value()) {
      return wh::core::result<wh::compose::graph>::failure(
          wh::core::errc::contract_violation);
    }

    auto toolset = authored_->tools();
    auto registry = toolset.registry();
    auto runtime_options = toolset.runtime_options();
    auto description = std::string{authored_->description()};
    auto instruction = authored_->render_instruction();
    auto output_key = std::string{authored_->output_key()};
    const auto output_mode = authored_->output_mode();
    const auto max_iterations = authored_->max_iterations();

    wh::compose::graph_compile_options compile_options{};
    compile_options.name = std::string{authored_->name()};
    compile_options.mode = wh::compose::graph_runtime_mode::pregel;
    compile_options.max_steps = max_iterations * 4U + 4U;
    compile_options.max_parallel_nodes = 1U;
    compile_options.max_parallel_per_node = 1U;
    wh::compose::graph lowered{std::move(compile_options)};

    auto bootstrap = wh::compose::make_lambda_node(
        "bootstrap",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        react_detail::make_bootstrap_options(max_iterations));
    auto bootstrap_added = lowered.add_lambda(std::move(bootstrap));
    if (bootstrap_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          bootstrap_added.error());
    }

    auto prepare_request = wh::compose::make_lambda_node(
        "prepare_request",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        react_detail::make_prepare_request_options(
            std::move(description), std::move(instruction),
            std::vector<wh::schema::tool_schema_definition>{
                toolset.schemas().begin(), toolset.schemas().end()}));
    auto prepare_request_added = lowered.add_lambda(std::move(prepare_request));
    if (prepare_request_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          prepare_request_added.error());
    }

    auto model_added = lowered.add_component(model_node.value().get());
    if (model_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_added.error());
    }

    auto prepare_tools = wh::compose::make_lambda_node<
        wh::compose::node_contract::stream, wh::compose::node_contract::value>(
        "prepare_tools",
        [toolset](wh::compose::graph_stream_reader input,
                  wh::core::run_context &,
                  const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto messages = react_detail::read_model_messages(std::move(input));
          if (messages.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                messages.error());
          }
          if (messages.value().empty()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::not_found);
          }
          const auto &assistant = messages.value().back();
          return wh::core::any(react_detail::prepared_tool_round{
              .assistant_message = assistant,
              .actions = react_detail::collect_tool_actions(assistant, toolset),
          });
        },
        react_detail::make_prepare_tools_options(toolset));
    auto prepare_tools_added = lowered.add_lambda(std::move(prepare_tools));
    if (prepare_tools_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          prepare_tools_added.error());
    }

    wh::core::result<void> tools_added{};
    if (tools_options->exec_mode == wh::compose::node_exec_mode::sync) {
      tools_added = lowered.add_tools(wh::compose::make_tools_node<
                                      wh::compose::node_contract::value,
                                      wh::compose::node_contract::value,
                                      wh::compose::node_exec_mode::sync>(
          "tools", std::move(registry), {}, std::move(runtime_options)));
    } else {
      tools_added = lowered.add_tools(wh::compose::make_tools_node<
                                      wh::compose::node_contract::value,
                                      wh::compose::node_contract::value,
                                      wh::compose::node_exec_mode::async>(
          "tools", std::move(registry), {}, std::move(runtime_options)));
    }
    if (tools_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(tools_added.error());
    }

    auto apply_tools = wh::compose::make_lambda_node(
        "apply_tools",
        [toolset](wh::compose::graph_value &input, wh::core::run_context &,
                  const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto *results =
              wh::core::any_cast<std::vector<wh::compose::tool_result>>(&input);
          if (results == nullptr) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::type_mismatch);
          }

          react_detail::apply_tools_decision decision{};
          decision.tool_messages.reserve(results->size());
          for (const auto &result : *results) {
            auto message = react_detail::tool_result_to_message(result);
            if (message.has_error()) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  message.error());
            }
            if (!decision.direct_return &&
                toolset.is_return_direct_tool(result.tool_name)) {
              decision.direct_return = true;
              decision.direct_message = message.value();
            }
            decision.tool_messages.push_back(std::move(message).value());
          }
          return wh::core::any(std::move(decision));
        },
        react_detail::make_apply_tools_options());
    auto apply_tools_added = lowered.add_lambda(std::move(apply_tools));
    if (apply_tools_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          apply_tools_added.error());
    }

    auto emit_direct = wh::compose::make_lambda_node(
        "emit_direct",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto *decision =
              wh::core::any_cast<react_detail::apply_tools_decision>(&input);
          if (decision == nullptr || !decision->direct_message.has_value()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::not_found);
          }
          return wh::core::any(*decision->direct_message);
        },
        react_detail::make_emit_direct_options(output_key, output_mode));
    auto emit_direct_added = lowered.add_lambda(std::move(emit_direct));
    if (emit_direct_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          emit_direct_added.error());
    }

    auto finalize = wh::compose::make_lambda_node<
        wh::compose::node_contract::stream, wh::compose::node_contract::value>(
        "finalize",
        [](wh::compose::graph_stream_reader input, wh::core::run_context &,
           const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto messages = react_detail::read_model_messages(std::move(input));
          if (messages.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                messages.error());
          }
          if (messages.value().empty()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::not_found);
          }
          return wh::core::any(messages.value().back());
        },
        react_detail::make_finalize_options(output_key, output_mode));
    auto finalize_added = lowered.add_lambda(std::move(finalize));
    if (finalize_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          finalize_added.error());
    }

    auto start_added = lowered.add_entry_edge("bootstrap");
    if (start_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(start_added.error());
    }
    auto bootstrap_edge = lowered.add_edge("bootstrap", "prepare_request");
    if (bootstrap_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          bootstrap_edge.error());
    }
    auto request_edge = lowered.add_edge(
        "prepare_request", std::string{wh::agent::react_model_node_key});
    if (request_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(request_edge.error());
    }
    auto prepare_tools_edge = lowered.add_edge("prepare_tools", "tools");
    if (prepare_tools_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          prepare_tools_edge.error());
    }
    auto apply_tools_edge = lowered.add_edge("tools", "apply_tools");
    if (apply_tools_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          apply_tools_edge.error());
    }
    auto emit_direct_edge = lowered.add_exit_edge("emit_direct");
    if (emit_direct_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          emit_direct_edge.error());
    }
    auto finalize_edge = lowered.add_exit_edge("finalize");
    if (finalize_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          finalize_edge.error());
    }

    auto prepare_tools_id = lowered.node_id("prepare_tools");
    if (prepare_tools_id.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          prepare_tools_id.error());
    }
    auto finalize_id = lowered.node_id("finalize");
    if (finalize_id.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finalize_id.error());
    }
    auto model_branch_added =
        lowered.add_stream_branch(wh::compose::graph_stream_branch{
            .from = std::string{wh::agent::react_model_node_key},
            .end_nodes = {"prepare_tools", "finalize"},
            .selector_ids =
                [prepare_tools_id = prepare_tools_id.value(),
                 finalize_id = finalize_id.value()](
                    wh::compose::graph_stream_reader input,
                    wh::core::run_context &,
                    const wh::compose::graph_call_scope &)
                    -> wh::core::result<std::vector<std::uint32_t>> {
                  auto messages =
                      react_detail::read_model_messages(std::move(input));
                  if (messages.has_error()) {
                    return wh::core::result<std::vector<std::uint32_t>>::failure(
                        messages.error());
                  }
                  for (const auto &message : messages.value()) {
                    for (const auto &part : message.parts) {
                      if (std::holds_alternative<wh::schema::tool_call_part>(
                              part)) {
                        return std::vector<std::uint32_t>{prepare_tools_id};
                      }
                    }
                  }
                  return std::vector<std::uint32_t>{finalize_id};
                },
        });
    if (model_branch_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          model_branch_added.error());
    }

    wh::compose::value_branch apply_branch{};
    auto add_loop_case = apply_branch.add_case(
        "prepare_request",
        [](const wh::compose::graph_value &output, wh::core::run_context &)
            -> wh::core::result<bool> {
          const auto *decision =
              wh::core::any_cast<react_detail::apply_tools_decision>(&output);
          if (decision == nullptr) {
            return wh::core::result<bool>::failure(
                wh::core::errc::type_mismatch);
          }
          return !decision->direct_return;
        });
    if (add_loop_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(add_loop_case.error());
    }
    auto add_direct_case = apply_branch.add_case(
        "emit_direct",
        [](const wh::compose::graph_value &output, wh::core::run_context &)
            -> wh::core::result<bool> {
          const auto *decision =
              wh::core::any_cast<react_detail::apply_tools_decision>(&output);
          if (decision == nullptr) {
            return wh::core::result<bool>::failure(
                wh::core::errc::type_mismatch);
          }
          return decision->direct_return;
        });
    if (add_direct_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          add_direct_case.error());
    }
    auto apply_branch_added = apply_branch.apply(lowered, "apply_tools");
    if (apply_branch_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          apply_branch_added.error());
    }

    return lowered;
  }

private:
  const wh::agent::react *authored_{nullptr};
};

/// Wraps one frozen ReAct shell as the common authored-agent surface.
[[nodiscard]] inline auto bind_react_agent(wh::agent::react authored)
    -> wh::core::result<wh::agent::agent> {
  auto frozen = authored.freeze();
  if (frozen.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(frozen.error());
  }

  wh::agent::agent exported{std::string{authored.name()}};
  auto described = exported.set_description(std::string{authored.description()});
  if (described.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(described.error());
  }

  auto shell = std::make_unique<wh::agent::react>(std::move(authored));
  auto bound = exported.bind_execution(
      nullptr,
      [shell = std::move(shell)]() mutable
          -> wh::core::result<wh::compose::graph> {
        return react_graph{*shell}.lower();
      });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  return exported;
}

} // namespace wh::adk::detail
