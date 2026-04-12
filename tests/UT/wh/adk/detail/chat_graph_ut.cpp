#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/detail/chat_graph.hpp"
#include "wh/compose/graph/stream.hpp"

namespace {

[[nodiscard]] auto invoke_chat_graph(
    wh::compose::graph &graph, std::vector<wh::schema::message> messages)
    -> wh::agent::agent_output {
  wh::compose::graph_invoke_request request{};
  request.input = wh::core::any{std::move(messages)};

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());

  auto invoke_status = std::get<0>(std::move(waited).value());
  REQUIRE(invoke_status.has_value());
  auto invoke_result = std::move(invoke_status).value();
  REQUIRE(invoke_result.output_status.has_value());

  auto *typed =
      wh::core::any_cast<wh::agent::agent_output>(&invoke_result.output_status.value());
  REQUIRE(typed != nullptr);
  return *typed;
}

} // namespace

TEST_CASE("chat detail helpers normalize message streams instructions and output slots",
          "[UT][wh/adk/detail/chat_graph.hpp][chat_detail::read_model_messages][condition][branch][boundary]") {
  auto stream = wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant,
                                             "first"),
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant,
                                             "second"),
  });
  REQUIRE(stream.has_value());

  auto messages =
      wh::adk::detail::chat_detail::read_model_messages(std::move(stream).value());
  REQUIRE(messages.has_value());
  REQUIRE(messages->size() == 2U);

  auto invalid_stream =
      wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
          wh::compose::graph_value{17},
      });
  REQUIRE(invalid_stream.has_value());
  auto invalid_messages = wh::adk::detail::chat_detail::read_model_messages(
      std::move(invalid_stream).value());
  REQUIRE(invalid_messages.has_error());
  REQUIRE(invalid_messages.error() == wh::core::errc::type_mismatch);

  auto instruction =
      wh::adk::detail::chat_detail::make_instruction_message("role", "prompt");
  REQUIRE(instruction.has_value());
  REQUIRE(instruction->role == wh::schema::message_role::system);
  REQUIRE(std::get<wh::schema::text_part>(instruction->parts.front()).text ==
          "role\nprompt");
  REQUIRE_FALSE(
      wh::adk::detail::chat_detail::make_instruction_message("", "").has_value());

  wh::schema::message rendered{};
  rendered.role = wh::schema::message_role::assistant;
  rendered.parts.emplace_back(wh::schema::text_part{"alpha"});
  rendered.parts.emplace_back(wh::schema::tool_call_part{
      .index = 0U,
      .id = "call-1",
      .type = "function",
      .name = "search",
      .arguments = "{}",
      .complete = true,
  });
  rendered.parts.emplace_back(wh::schema::text_part{"beta"});
  REQUIRE(wh::adk::detail::chat_detail::render_message_text(rendered) ==
          "alphabeta");

  wh::agent::agent_output output{};
  wh::adk::detail::chat_detail::write_output_value(
      output, "", wh::agent::chat_output_mode::value, rendered);
  REQUIRE(output.output_values.empty());

  wh::adk::detail::chat_detail::write_output_value(
      output, "message", wh::agent::chat_output_mode::value, rendered);
  auto message_iter = output.output_values.find("message");
  REQUIRE(message_iter != output.output_values.end());
  REQUIRE(wh::core::any_cast<wh::schema::message>(&message_iter->second) != nullptr);

  wh::adk::detail::chat_detail::write_output_value(
      output, "text", wh::agent::chat_output_mode::text, rendered);
  auto text_iter = output.output_values.find("text");
  REQUIRE(text_iter != output.output_values.end());
  auto *text = wh::core::any_cast<std::string>(&text_iter->second);
  REQUIRE(text != nullptr);
  REQUIRE(*text == "alphabeta");
}

TEST_CASE("chat graph lowers authored chat shells into executable compose graphs and binders",
          "[UT][wh/adk/detail/chat_graph.hpp][chat_graph::lower][condition][branch][boundary]") {
  wh::agent::chat missing_model{"chat", "assistant"};
  auto missing_lower = wh::adk::detail::chat_graph{missing_model}.lower();
  REQUIRE(missing_lower.has_error());
  REQUIRE(missing_lower.error() == wh::core::errc::not_found);

  auto authored = wh::testing::helper::make_configured_chat("chat", "assistant");
  REQUIRE(authored.append_instruction("follow policy").has_value());
  REQUIRE(authored.set_output_key("reply").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::chat_output_mode::text).has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = wh::adk::detail::chat_graph{authored}.lower();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->compiled());

  auto output = invoke_chat_graph(
      lowered.value(),
      {wh::testing::helper::make_text_message(wh::schema::message_role::user,
                                              "hello")});
  REQUIRE(output.final_message.role == wh::schema::message_role::assistant);
  REQUIRE(output.history_messages.size() == 1U);
  auto reply_iter = output.output_values.find("reply");
  REQUIRE(reply_iter != output.output_values.end());
  auto *reply = wh::core::any_cast<std::string>(&reply_iter->second);
  REQUIRE(reply != nullptr);
  REQUIRE(*reply == "ok");

  auto bound = wh::adk::detail::bind_chat_agent(
      wh::testing::helper::make_configured_chat("bound-chat", "assistant"));
  REQUIRE(bound.has_value());
  REQUIRE(bound->name() == "bound-chat");
  auto bound_graph = bound->lower_graph();
  REQUIRE(bound_graph.has_value());
  REQUIRE(bound_graph->compiled());
}
