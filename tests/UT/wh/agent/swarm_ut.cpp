#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/swarm.hpp"

TEST_CASE("swarm shell binds host peers validates names and lowers into executable agent",
          "[UT][wh/agent/swarm.hpp][swarm::freeze][condition][branch][boundary]") {
  wh::agent::swarm authored{"host"};
  REQUIRE(authored.name() == "host");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.host_agent().has_error());
  REQUIRE(authored.peers().empty());

  auto host = wh::testing::helper::make_executable_agent("host");
  auto peer = wh::testing::helper::make_executable_agent("peer");
  REQUIRE(host.has_value());
  REQUIRE(peer.has_value());
  REQUIRE(authored.set_host(std::move(host).value()).has_value());
  REQUIRE(authored.add_peer(std::move(peer).value()).has_value());
  REQUIRE(authored.host_agent().has_value());
  REQUIRE(authored.peer_names() == std::vector<std::string>{"peer"});
  REQUIRE(authored.peers().size() == 1U);
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().executable());
}

TEST_CASE("swarm shell rejects invalid role topology duplicates and late mutation",
          "[UT][wh/agent/swarm.hpp][swarm::add_peer][condition][branch][boundary]") {
  wh::agent::swarm missing{"host"};
  auto missing_freeze = missing.freeze();
  REQUIRE(missing_freeze.has_error());
  REQUIRE(missing_freeze.error() == wh::core::errc::invalid_argument);
  REQUIRE(missing.set_host(wh::agent::agent{""}).has_error());
  REQUIRE(missing.add_peer(wh::agent::agent{""}).has_error());

  wh::agent::swarm duplicate{"host"};
  auto host = wh::testing::helper::make_executable_agent("host");
  auto peer_a = wh::testing::helper::make_executable_agent("dup");
  auto peer_b = wh::testing::helper::make_executable_agent("dup");
  REQUIRE(host.has_value());
  REQUIRE(peer_a.has_value());
  REQUIRE(peer_b.has_value());
  REQUIRE(duplicate.set_host(std::move(host).value()).has_value());
  REQUIRE(duplicate.add_peer(std::move(peer_a).value()).has_value());
  REQUIRE(duplicate.add_peer(std::move(peer_b).value()).has_value());
  auto duplicate_freeze = duplicate.freeze();
  REQUIRE(duplicate_freeze.has_error());
  REQUIRE(duplicate_freeze.error() == wh::core::errc::already_exists);

  auto configured = wh::testing::helper::make_configured_swarm("configured");
  REQUIRE(configured.has_value());
  REQUIRE(configured->freeze().has_value());
  REQUIRE(configured->set_host(wh::agent::agent{"late"}).has_error());
  REQUIRE(configured->add_peer(wh::agent::agent{"late"}).has_error());
}
