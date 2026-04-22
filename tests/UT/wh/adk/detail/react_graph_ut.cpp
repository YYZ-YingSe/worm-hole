#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/detail/history_request.hpp"
#include "wh/adk/detail/react_graph.hpp"
#include "wh/compose/graph/stream.hpp"

namespace {

[[nodiscard]] auto run_pre(wh::compose::graph_add_node_options &options,
                           wh::compose::graph_process_state &process_state,
                           wh::compose::graph_value &payload) -> wh::core::result<void> {
  REQUIRE(static_cast<bool>(options.state.pre().handler));
  wh::compose::graph_state_cause cause{};
  wh::core::run_context context{};
  return options.state.pre().handler(cause, process_state, payload, context);
}

[[nodiscard]] auto run_post(wh::compose::graph_add_node_options &options,
                            wh::compose::graph_process_state &process_state,
                            wh::compose::graph_value &payload) -> wh::core::result<void> {
  REQUIRE(static_cast<bool>(options.state.post().handler));
  wh::compose::graph_state_cause cause{};
  wh::core::run_context context{};
  return options.state.post().handler(cause, process_state, payload, context);
}

[[nodiscard]] auto invoke_react_graph(wh::compose::graph &graph,
                                      std::vector<wh::schema::message> messages)
    -> wh::agent::agent_output {
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(wh::core::any{std::move(messages)});

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());

  auto invoke_status = std::get<0>(std::move(waited).value());
  REQUIRE(invoke_status.has_value());
  auto invoke_result = std::move(invoke_status).value();
  REQUIRE(invoke_result.output_status.has_value());

  auto *typed = wh::core::any_cast<wh::agent::agent_output>(&invoke_result.output_status.value());
  REQUIRE(typed != nullptr);
  return *typed;
}

} // namespace

TEST_CASE(
    "react detail helpers normalize model messages tool actions tool batches and output projection",
    "[UT][wh/adk/detail/"
    "react_graph.hpp][react_detail::collect_tool_actions][condition][branch][boundary]") {
  auto stream = wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "draft"),
  });
  REQUIRE(stream.has_value());
  auto messages = wh::adk::detail::react_detail::read_model_messages(std::move(stream).value());
  REQUIRE(messages.has_value());
  REQUIRE(messages->size() == 1U);

  auto invalid_stream =
      wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
          wh::compose::graph_value{9},
      });
  REQUIRE(invalid_stream.has_value());
  auto invalid_messages =
      wh::adk::detail::react_detail::read_model_messages(std::move(invalid_stream).value());
  REQUIRE(invalid_messages.has_error());
  REQUIRE(invalid_messages.error() == wh::core::errc::type_mismatch);

  auto instruction = wh::adk::detail::react_detail::make_instruction_message("role", "guide");
  REQUIRE(instruction.has_value());
  REQUIRE_FALSE(wh::adk::detail::react_detail::make_instruction_message("", "").has_value());

  wh::agent::react authored{"react", "assistant"};
  wh::schema::tool_schema_definition schema{
      .name = "search",
      .description = "search tool",
  };
  wh::compose::tool_entry entry{};
  entry.invoke = [](const wh::compose::tool_call &call,
                    const wh::tool::call_scope &) -> wh::core::result<wh::compose::graph_value> {
    return wh::compose::graph_value{call.arguments};
  };
  REQUIRE(authored.add_tool_entry(schema, entry, {.return_direct = true}).has_value());

  wh::schema::message assistant{};
  assistant.role = wh::schema::message_role::assistant;
  assistant.parts.emplace_back(wh::schema::text_part{"alpha"});
  assistant.parts.emplace_back(wh::schema::tool_call_part{
      .index = 0U,
      .id = "call-1",
      .type = "function",
      .name = "search",
      .arguments = "{\"q\":\"hi\"}",
      .complete = true,
  });
  assistant.parts.emplace_back(wh::schema::tool_call_part{
      .index = 1U,
      .id = "call-2",
      .type = "function",
      .name = "other",
      .arguments = "{}",
      .complete = true,
  });

  auto actions = wh::adk::detail::react_detail::collect_tool_actions(assistant, authored.tools());
  REQUIRE(actions.size() == 2U);
  REQUIRE(actions.front().return_direct);
  REQUIRE_FALSE(actions.back().return_direct);

  auto batch = wh::adk::detail::react_detail::make_tool_batch(actions);
  REQUIRE(batch.calls.size() == 2U);
  REQUIRE(batch.calls.front().tool_name == "search");

  wh::model::chat_request history_request{};
  history_request.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "history"));
  auto batch_with_history =
      wh::adk::detail::react_detail::make_tool_batch(actions, history_request);
  REQUIRE(batch_with_history.calls.size() == 2U);
  auto payload_view =
      wh::adk::detail::read_history_request_payload_view(batch_with_history.calls.front().payload);
  REQUIRE(payload_view.has_value());
  REQUIRE(payload_view->history_request->messages.size() == 1U);

  wh::compose::tool_result string_result{
      .call_id = "call-1",
      .tool_name = "search",
      .value = wh::core::any{std::string{"result"}},
  };
  auto tool_message = wh::adk::detail::react_detail::tool_result_to_message(string_result);
  REQUIRE(tool_message.has_value());
  REQUIRE(tool_message->role == wh::schema::message_role::tool);
  REQUIRE(tool_message->tool_call_id == "call-1");

  wh::compose::tool_result typed_result{
      .call_id = "call-2",
      .tool_name = "other",
      .value = wh::core::any{wh::schema::message{
          .role = wh::schema::message_role::assistant,
          .parts = {wh::schema::text_part{"typed"}},
      }},
  };
  auto typed_message = wh::adk::detail::react_detail::tool_result_to_message(typed_result);
  REQUIRE(typed_message.has_value());
  REQUIRE(typed_message->role == wh::schema::message_role::tool);
  REQUIRE(typed_message->tool_name == "other");

  wh::compose::tool_result invalid_result{
      .call_id = "call-3",
      .tool_name = "bad",
      .value = wh::core::any{3},
  };
  auto invalid_message = wh::adk::detail::react_detail::tool_result_to_message(invalid_result);
  REQUIRE(invalid_message.has_error());
  REQUIRE(invalid_message.error() == wh::core::errc::type_mismatch);

  wh::agent::react_state state{};
  wh::adk::detail::react_detail::write_output_value(state, "value",
                                                    wh::agent::react_output_mode::value, assistant);
  wh::adk::detail::react_detail::write_output_value(
      state, "text", wh::agent::react_output_mode::stream, assistant);
  REQUIRE(state.output_values.find("value") != state.output_values.end());
  auto text_iter = state.output_values.find("text");
  REQUIRE(text_iter != state.output_values.end());
  auto *text = wh::core::any_cast<std::string>(&text_iter->second);
  REQUIRE(text != nullptr);
  REQUIRE(*text == "alpha");

  state.messages.push_back(assistant);
  auto output = wh::adk::detail::react_detail::make_agent_output(state, assistant);
  REQUIRE(output.history_messages.size() == 1U);
  REQUIRE(output.output_values.size() == 2U);
}

TEST_CASE(
    "react state callbacks bootstrap request tool application direct emit and finalize branches",
    "[UT][wh/adk/detail/"
    "react_graph.hpp][react_detail::make_bootstrap_options][condition][branch][boundary]") {
  wh::compose::graph_process_state process_state{};
  wh::compose::graph_value payload = std::vector<wh::schema::message>{
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "seed"),
  };

  auto bootstrap = wh::adk::detail::react_detail::make_bootstrap_options(2U);
  REQUIRE(run_pre(bootstrap, process_state, payload).has_value());
  auto state = process_state.workflow_state_ref<wh::agent::react_state>();
  REQUIRE(state.has_value());
  REQUIRE(state->get().messages.size() == 1U);
  REQUIRE(state->get().remaining_iterations == 2U);

  auto prepare_request = wh::adk::detail::react_detail::make_prepare_request_options(
      "desc", "inst", std::vector<wh::schema::tool_schema_definition>{});
  REQUIRE(run_pre(prepare_request, process_state, payload).has_value());
  auto *request = wh::core::any_cast<wh::model::chat_request>(&payload);
  REQUIRE(request != nullptr);
  REQUIRE(request->messages.size() == 2U);
  REQUIRE(request->tools.empty());

  state->get().remaining_iterations = 0U;
  auto exhausted = run_pre(prepare_request, process_state, payload);
  REQUIRE(exhausted.has_error());
  REQUIRE(exhausted.error() == wh::core::errc::resource_exhausted);
  state->get().remaining_iterations = 2U;

  auto prepare_tools = wh::adk::detail::react_detail::make_prepare_tools_options(
      wh::testing::helper::make_configured_react("tool-react", "assistant").tools());
  wh::adk::detail::react_detail::prepared_tool_round prepared{
      .assistant_message =
          wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "tool-call"),
      .actions = {wh::agent::react_tool_action{
          .call_id = "call-1",
          .tool_name = "search",
          .arguments = "{}",
          .return_direct = false,
      }},
  };
  payload = prepared;
  REQUIRE(run_post(prepare_tools, process_state, payload).has_value());
  auto *prepared_batch = wh::core::any_cast<wh::compose::tool_batch>(&payload);
  REQUIRE(prepared_batch != nullptr);
  REQUIRE(prepared_batch->calls.size() == 1U);
  REQUIRE(state->get().messages.size() == 2U);
  REQUIRE(state->get().pending_tool_actions.size() == 1U);
  REQUIRE(state->get().remaining_iterations == 1U);

  wh::adk::detail::react_detail::apply_tools_decision decision{
      .direct_return = true,
      .direct_message =
          wh::schema::message{
              .role = wh::schema::message_role::tool,
              .tool_call_id = "call-1",
              .parts = {wh::schema::text_part{"direct"}},
          },
      .tool_messages = {wh::schema::message{
          .role = wh::schema::message_role::tool,
          .tool_call_id = "call-1",
          .parts = {wh::schema::text_part{"direct"}},
      }},
  };
  payload = decision;
  auto apply = wh::adk::detail::react_detail::make_apply_tools_options();
  REQUIRE(run_post(apply, process_state, payload).has_value());
  REQUIRE(state->get().messages.size() == 3U);
  REQUIRE(state->get().pending_tool_actions.empty());
  REQUIRE(state->get().return_direct_call_id == std::optional<std::string>{"call-1"});

  payload = decision.direct_message.value();
  auto emit_direct = wh::adk::detail::react_detail::make_emit_direct_options(
      "direct", wh::agent::react_output_mode::stream);
  REQUIRE(run_post(emit_direct, process_state, payload).has_value());
  auto *direct_output = wh::core::any_cast<wh::agent::agent_output>(&payload);
  REQUIRE(direct_output != nullptr);
  REQUIRE(direct_output->output_values.find("direct") != direct_output->output_values.end());

  state->get().remaining_iterations = 1U;
  payload = wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "done");
  auto finalize = wh::adk::detail::react_detail::make_finalize_options(
      "final", wh::agent::react_output_mode::stream);
  REQUIRE(run_post(finalize, process_state, payload).has_value());
  auto *final_output = wh::core::any_cast<wh::agent::agent_output>(&payload);
  REQUIRE(final_output != nullptr);
  REQUIRE(state->get().remaining_iterations == 0U);
  REQUIRE(final_output->output_values.find("final") != final_output->output_values.end());

  auto missing_state_payload = wh::compose::graph_value{std::monostate{}};
  wh::compose::graph_process_state missing_state{};
  auto missing_direct = run_post(emit_direct, missing_state, missing_state_payload);
  REQUIRE(missing_direct.has_error());
}

TEST_CASE("react graph lowers authored shells into executable graphs and binders",
          "[UT][wh/adk/detail/react_graph.hpp][bind_react_agent][condition][branch][boundary]") {
  wh::agent::react invalid{"react", "assistant"};
  auto invalid_lower = wh::adk::detail::react_graph{invalid}.lower();
  REQUIRE(invalid_lower.has_error());
  REQUIRE(invalid_lower.error() == wh::core::errc::not_found);

  auto authored = wh::testing::helper::make_configured_react("react", "assistant");
  REQUIRE(authored.set_output_key("final").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::react_output_mode::stream).has_value());
  auto lowered = wh::adk::detail::react_graph{authored}.lower();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->compile().has_value());

  auto output = invoke_react_graph(lowered.value(), {wh::testing::helper::make_text_message(
                                                        wh::schema::message_role::user, "hello")});
  REQUIRE(output.final_message.role == wh::schema::message_role::assistant);
  REQUIRE(output.history_messages.size() == 2U);
  auto final_iter = output.output_values.find("final");
  REQUIRE(final_iter != output.output_values.end());
  auto *final_text = wh::core::any_cast<std::string>(&final_iter->second);
  REQUIRE(final_text != nullptr);
  REQUIRE(*final_text == "ok");

  auto bound_authored = wh::testing::helper::make_configured_react("bound-react", "assistant");
  REQUIRE(bound_authored.freeze().has_value());
  auto bound = wh::adk::detail::bind_react_agent(std::move(bound_authored));
  REQUIRE(bound.has_value());
  auto bound_graph = bound->lower();
  REQUIRE(bound_graph.has_value());
  REQUIRE(bound_graph->compile().has_value());
}
