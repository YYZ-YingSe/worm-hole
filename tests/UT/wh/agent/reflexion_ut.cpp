#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/reflexion.hpp"

TEST_CASE(
    "reflexion shell binds actor critic optional memory writer and lowers into executable agent",
    "[UT][wh/agent/reflexion.hpp][reflexion::freeze][condition][branch][boundary]") {
  wh::agent::reflexion authored{"reflexion"};
  REQUIRE(authored.name() == "reflexion");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.max_iterations() == 3U);
  REQUIRE(authored.actor().has_error());
  REQUIRE(authored.critic().has_error());
  REQUIRE(authored.memory_writer().has_error());

  REQUIRE(authored.set_max_iterations(0U).has_value());
  REQUIRE(authored.max_iterations() == 1U);

  auto actor = wh::testing::helper::make_executable_agent("actor");
  auto critic = wh::testing::helper::make_executable_agent("critic");
  auto memory_writer = wh::testing::helper::make_executable_agent("memory");
  REQUIRE(actor.has_value());
  REQUIRE(critic.has_value());
  REQUIRE(memory_writer.has_value());
  REQUIRE(authored.set_actor(std::move(actor).value()).has_value());
  REQUIRE(authored.set_critic(std::move(critic).value()).has_value());
  REQUIRE(authored.set_memory_writer(std::move(memory_writer).value()).has_value());
  REQUIRE(authored.actor().has_value());
  REQUIRE(authored.critic().has_value());
  REQUIRE(authored.memory_writer().has_value());

  REQUIRE(authored.set_actor_request_builder(wh::testing::helper::make_revision_request_builder())
              .has_value());
  REQUIRE(authored.set_critic_request_builder(wh::testing::helper::make_revision_request_builder())
              .has_value());
  REQUIRE(
      authored
          .set_memory_writer_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_value());
  REQUIRE(authored
              .set_review_decision_reader(wh::testing::helper::make_review_decision_reader(
                  wh::agent::review_decision_kind::accept))
              .has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().executable());
}

TEST_CASE(
    "reflexion shell enforces builder role parity executability and late mutation",
    "[UT][wh/agent/"
    "reflexion.hpp][reflexion::set_memory_writer_request_builder][condition][branch][boundary]") {
  wh::agent::reflexion missing{"missing"};
  auto missing_freeze = missing.freeze();
  REQUIRE(missing_freeze.has_error());
  REQUIRE(missing_freeze.error() == wh::core::errc::invalid_argument);

  REQUIRE(
      missing.set_actor_request_builder(wh::agent::revision_request_builder{nullptr}).has_error());
  REQUIRE(
      missing.set_critic_request_builder(wh::agent::revision_request_builder{nullptr}).has_error());
  REQUIRE(missing.set_memory_writer_request_builder(wh::agent::revision_request_builder{nullptr})
              .has_error());
  REQUIRE(
      missing.set_review_decision_reader(wh::agent::review_decision_reader{nullptr}).has_error());
  REQUIRE(missing.set_actor(wh::agent::agent{""}).has_error());
  REQUIRE(missing.set_critic(wh::agent::agent{""}).has_error());
  REQUIRE(missing.set_memory_writer(wh::agent::agent{""}).has_error());

  wh::agent::reflexion parity{"parity"};
  auto actor = wh::testing::helper::make_executable_agent("actor");
  auto critic = wh::testing::helper::make_executable_agent("critic");
  REQUIRE(actor.has_value());
  REQUIRE(critic.has_value());
  REQUIRE(parity.set_actor(std::move(actor).value()).has_value());
  REQUIRE(parity.set_critic(std::move(critic).value()).has_value());
  REQUIRE(parity.set_actor_request_builder(wh::testing::helper::make_revision_request_builder())
              .has_value());
  REQUIRE(parity.set_critic_request_builder(wh::testing::helper::make_revision_request_builder())
              .has_value());
  REQUIRE(parity
              .set_review_decision_reader(wh::testing::helper::make_review_decision_reader(
                  wh::agent::review_decision_kind::accept))
              .has_value());
  REQUIRE(
      parity.set_memory_writer_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_value());
  auto parity_freeze = parity.freeze();
  REQUIRE(parity_freeze.has_error());
  REQUIRE(parity_freeze.error() == wh::core::errc::contract_violation);

  auto configured = wh::testing::helper::make_configured_reflexion("configured");
  REQUIRE(configured.has_value());
  REQUIRE(configured->freeze().has_value());
  REQUIRE(configured->set_max_iterations(2U).has_error());
  REQUIRE(
      configured->set_actor_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_error());
  REQUIRE(configured->set_actor(wh::agent::agent{"late"}).has_error());
}
