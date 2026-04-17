#include <catch2/catch_test_macros.hpp>

#include <string>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/deterministic_transfer.hpp"
#include "wh/agent/bind.hpp"

namespace {

[[nodiscard]] auto make_user_message(const std::string &text)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

[[nodiscard]] auto make_assistant_message(const std::string &name,
                                          const std::string &text)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.name = name;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

using wh::testing::helper::invoke_agent_graph;
using wh::testing::helper::make_fixed_output_agent;
using wh::testing::helper::message_text;

} // namespace

TEST_CASE("supervisor host graph executes root transfer into child subgraph",
          "[core][agent][host_graph]") {
  wh::agent::agent_output supervisor_output{};
  supervisor_output.final_message =
      wh::adk::make_transfer_tool_message("planner", "call-1");
  supervisor_output.history_messages.push_back(
      wh::adk::make_transfer_assistant_message("planner", "call-1"));
  supervisor_output.history_messages.push_back(
      wh::adk::make_transfer_tool_message("planner", "call-1"));
  supervisor_output.transfer = wh::agent::agent_transfer{
      .target_agent_name = "planner",
      .tool_call_id = "call-1",
  };

  wh::agent::agent_output worker_output{};
  worker_output.final_message = make_assistant_message("planner", "done");
  worker_output.history_messages.push_back(worker_output.final_message);

  auto supervisor_agent =
      make_fixed_output_agent("supervisor", std::move(supervisor_output));
  REQUIRE(supervisor_agent.has_value());
  auto planner_agent =
      make_fixed_output_agent("planner", std::move(worker_output));
  REQUIRE(planner_agent.has_value());

  wh::agent::supervisor authored{"supervisor"};
  REQUIRE(authored.set_supervisor(std::move(supervisor_agent).value()).has_value());
  REQUIRE(authored.add_worker(std::move(planner_agent).value()).has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().allows_transfer_to_child("planner"));
  auto planner = lowered.value().child("planner");
  REQUIRE(planner.has_value());
  REQUIRE(planner.value().get().allows_transfer_to_parent());

  auto host_graph = lowered.value().lower();
  REQUIRE(host_graph.has_value());
  auto output = invoke_agent_graph(host_graph.value(), {make_user_message("hello")});
  REQUIRE(output.has_value());

  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.name == "planner");
  REQUIRE(message_text(output->final_message) == "done");
  REQUIRE(output->history_messages.size() == 3U);
  REQUIRE(output->history_messages[0].name == "supervisor");
  REQUIRE(output->history_messages[1].name == "supervisor");
  REQUIRE(output->history_messages[2].name == "planner");
}
