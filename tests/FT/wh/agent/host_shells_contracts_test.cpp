#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/deterministic_transfer.hpp"
#include "wh/agent/bind.hpp"

TEST_CASE("research shell public binding executes lead transfer into specialist",
          "[core][agent][research][functional]") {
  wh::agent::agent_output lead_output{};
  lead_output.final_message =
      wh::adk::make_transfer_tool_message("specialist", "call-1");
  lead_output.history_messages.push_back(
      wh::adk::make_transfer_assistant_message("specialist", "call-1"));
  lead_output.history_messages.push_back(
      wh::adk::make_transfer_tool_message("specialist", "call-1"));
  lead_output.transfer = wh::agent::agent_transfer{
      .target_agent_name = "specialist",
      .tool_call_id = "call-1",
  };

  wh::agent::agent_output specialist_output{};
  specialist_output.final_message = wh::testing::helper::make_text_message(
      wh::schema::message_role::assistant, "research done");
  specialist_output.history_messages.push_back(specialist_output.final_message);

  auto lead =
      wh::testing::helper::make_fixed_output_agent("research", std::move(lead_output));
  REQUIRE(lead.has_value());
  auto specialist = wh::testing::helper::make_fixed_output_agent(
      "specialist", std::move(specialist_output));
  REQUIRE(specialist.has_value());

  wh::agent::research authored{"research"};
  REQUIRE(authored.set_lead(std::move(*lead)).has_value());
  REQUIRE(authored.add_specialist(std::move(*specialist)).has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->executable());

  auto graph = lowered->lower();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = wh::testing::helper::invoke_agent_graph(
      graph.value(),
      {wh::testing::helper::make_text_message(wh::schema::message_role::user,
                                              "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.name == "specialist");
  REQUIRE(wh::testing::helper::message_text(output->final_message) ==
          "research done");
}

TEST_CASE("swarm shell public binding executes host transfer into peer",
          "[core][agent][swarm][functional]") {
  wh::agent::agent_output host_output{};
  host_output.final_message =
      wh::adk::make_transfer_tool_message("peer", "call-1");
  host_output.history_messages.push_back(
      wh::adk::make_transfer_assistant_message("peer", "call-1"));
  host_output.history_messages.push_back(
      wh::adk::make_transfer_tool_message("peer", "call-1"));
  host_output.transfer = wh::agent::agent_transfer{
      .target_agent_name = "peer",
      .tool_call_id = "call-1",
  };

  wh::agent::agent_output peer_output{};
  peer_output.final_message = wh::testing::helper::make_text_message(
      wh::schema::message_role::assistant, "swarm done");
  peer_output.history_messages.push_back(peer_output.final_message);

  auto host =
      wh::testing::helper::make_fixed_output_agent("swarm", std::move(host_output));
  REQUIRE(host.has_value());
  auto peer =
      wh::testing::helper::make_fixed_output_agent("peer", std::move(peer_output));
  REQUIRE(peer.has_value());

  wh::agent::swarm authored{"swarm"};
  REQUIRE(authored.set_host(std::move(*host)).has_value());
  REQUIRE(authored.add_peer(std::move(*peer)).has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->executable());

  auto graph = lowered->lower();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = wh::testing::helper::invoke_agent_graph(
      graph.value(),
      {wh::testing::helper::make_text_message(wh::schema::message_role::user,
                                              "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.name == "peer");
  REQUIRE(wh::testing::helper::message_text(output->final_message) ==
          "swarm done");
}
