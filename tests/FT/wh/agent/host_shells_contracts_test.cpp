#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/deterministic_transfer.hpp"
#include "wh/agent/bind.hpp"

TEST_CASE("research shell public binding executes lead transfer into specialist",
          "[core][agent][research][functional]") {
  auto lead = wh::testing::helper::make_executable_message_agent(
      "research", wh::compose::node_contract::value, wh::compose::node_contract::value,
      wh::adk::make_transfer_tool_message("specialist", "call-1"));
  REQUIRE(lead.has_value());
  auto specialist = wh::testing::helper::make_executable_message_agent(
      "specialist", wh::compose::node_contract::stream, wh::compose::node_contract::stream,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "research done"));
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
      {wh::testing::helper::make_text_message(wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.name == "specialist");
  REQUIRE(wh::testing::helper::message_text(output->final_message) == "research done");
}

TEST_CASE("swarm shell public binding executes host transfer into peer",
          "[core][agent][swarm][functional]") {
  auto host = wh::testing::helper::make_executable_message_agent(
      "swarm", wh::compose::node_contract::stream, wh::compose::node_contract::stream,
      wh::adk::make_transfer_tool_message("peer", "call-1"));
  REQUIRE(host.has_value());
  auto peer = wh::testing::helper::make_executable_message_agent(
      "peer", wh::compose::node_contract::value, wh::compose::node_contract::value,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "swarm done"));
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
      {wh::testing::helper::make_text_message(wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.name == "peer");
  REQUIRE(wh::testing::helper::message_text(output->final_message) == "swarm done");
}
