#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"

namespace {

using wh::testing::helper::invoke_agent_graph;

} // namespace

TEST_CASE("chat shell public binding lowers and executes final output",
          "[core][agent][chat][functional]") {
  auto authored = wh::testing::helper::make_configured_chat("chat", "assistant");
  REQUIRE(authored.append_instruction("follow policy").has_value());
  REQUIRE(authored.set_output_key("reply").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::chat_output_mode::text).has_value());

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->executable());

  auto graph = lowered->lower_graph();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(
      graph.value(),
      {wh::testing::helper::make_text_message(wh::schema::message_role::user,
                                              "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(output->history_messages.size() == 1U);
  auto reply_iter = output->output_values.find("reply");
  REQUIRE(reply_iter != output->output_values.end());
  auto *reply = wh::core::any_cast<std::string>(&reply_iter->second);
  REQUIRE(reply != nullptr);
  REQUIRE(*reply == "ok");
}

TEST_CASE("react shell public binding lowers and executes tool-capable final output",
          "[core][agent][react][functional]") {
  auto authored =
      wh::testing::helper::make_configured_react("react", "assistant");
  REQUIRE(authored.set_output_key("final").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::react_output_mode::stream)
              .has_value());

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->executable());

  auto graph = lowered->lower_graph();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(
      graph.value(),
      {wh::testing::helper::make_text_message(wh::schema::message_role::user,
                                              "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(output->history_messages.size() == 2U);
  auto final_iter = output->output_values.find("final");
  REQUIRE(final_iter != output->output_values.end());
  auto *final_text = wh::core::any_cast<std::string>(&final_iter->second);
  REQUIRE(final_text != nullptr);
  REQUIRE(*final_text == "ok");
}
