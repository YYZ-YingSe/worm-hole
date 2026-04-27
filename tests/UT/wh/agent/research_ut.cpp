#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/research.hpp"

TEST_CASE("research shell binds lead specialists validates names and lowers into executable agent",
          "[UT][wh/agent/research.hpp][research::freeze][condition][branch][boundary]") {
  wh::agent::research authored{"lead"};
  REQUIRE(authored.name() == "lead");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.lead().has_error());
  REQUIRE(authored.specialists().empty());

  auto lead = wh::testing::helper::make_executable_agent("lead");
  auto specialist = wh::testing::helper::make_executable_agent("specialist");
  REQUIRE(lead.has_value());
  REQUIRE(specialist.has_value());
  REQUIRE(authored.set_lead(std::move(lead).value()).has_value());
  REQUIRE(authored.add_specialist(std::move(specialist).value()).has_value());
  REQUIRE(authored.lead().has_value());
  REQUIRE(authored.specialist_names() == std::vector<std::string>{"specialist"});
  REQUIRE(authored.specialists().size() == 1U);
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().executable());
}

TEST_CASE("research shell accepts authored role providers directly",
          "[UT][wh/agent/research.hpp][research::set_lead][surface][role_binding]") {
  wh::agent::research authored{"lead-authored"};
  REQUIRE(
      authored.set_lead(wh::testing::helper::make_configured_chat("lead-authored", "lead-authored"))
          .has_value());
  REQUIRE(
      authored
          .add_specialist(wh::testing::helper::make_configured_react("specialist", "specialist"))
          .has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lower().has_value());
}

TEST_CASE("research shell rejects invalid lead specialist topology and late mutation",
          "[UT][wh/agent/research.hpp][research::add_specialist][condition][branch][boundary]") {
  wh::agent::research missing{"lead"};
  auto missing_freeze = missing.freeze();
  REQUIRE(missing_freeze.has_error());
  REQUIRE(missing_freeze.error() == wh::core::errc::invalid_argument);
  REQUIRE(missing.set_lead(wh::agent::agent{""}).has_error());
  REQUIRE(missing.add_specialist(wh::agent::agent{""}).has_error());

  wh::agent::research wrong_name{"lead"};
  auto foreign_lead = wh::testing::helper::make_executable_agent("other");
  REQUIRE(foreign_lead.has_value());
  REQUIRE(wrong_name.set_lead(std::move(foreign_lead).value()).has_value());
  auto wrong_name_freeze = wrong_name.freeze();
  REQUIRE(wrong_name_freeze.has_error());
  REQUIRE(wrong_name_freeze.error() == wh::core::errc::contract_violation);

  wh::agent::research duplicate{"lead"};
  auto lead = wh::testing::helper::make_executable_agent("lead");
  auto specialist_a = wh::testing::helper::make_executable_agent("dup");
  auto specialist_b = wh::testing::helper::make_executable_agent("dup");
  REQUIRE(lead.has_value());
  REQUIRE(specialist_a.has_value());
  REQUIRE(specialist_b.has_value());
  REQUIRE(duplicate.set_lead(std::move(lead).value()).has_value());
  REQUIRE(duplicate.add_specialist(std::move(specialist_a).value()).has_value());
  REQUIRE(duplicate.add_specialist(std::move(specialist_b).value()).has_value());
  auto duplicate_freeze = duplicate.freeze();
  REQUIRE(duplicate_freeze.has_error());
  REQUIRE(duplicate_freeze.error() == wh::core::errc::already_exists);

  auto configured = wh::testing::helper::make_configured_research("configured");
  REQUIRE(configured.has_value());
  REQUIRE(configured->freeze().has_value());
  REQUIRE(configured->set_lead(wh::agent::agent{"late"}).has_error());
  REQUIRE(configured->add_specialist(wh::agent::agent{"late"}).has_error());
}
