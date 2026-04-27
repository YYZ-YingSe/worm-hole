#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/reviewer_executor.hpp"

TEST_CASE(
    "reviewer executor shell binds reviewer executor builders and lowers into executable agent",
    "[UT][wh/agent/"
    "reviewer_executor.hpp][reviewer_executor::freeze][condition][branch][boundary]") {
  wh::agent::reviewer_executor authored{"reviewer-executor"};
  REQUIRE(authored.name() == "reviewer-executor");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.max_iterations() == 3U);
  REQUIRE(authored.reviewer().has_error());
  REQUIRE(authored.executor().has_error());

  REQUIRE(authored.set_max_iterations(0U).has_value());
  REQUIRE(authored.max_iterations() == 1U);

  auto reviewer = wh::testing::helper::make_executable_agent("reviewer");
  auto executor = wh::testing::helper::make_executable_agent("executor");
  REQUIRE(reviewer.has_value());
  REQUIRE(executor.has_value());
  REQUIRE(authored.set_reviewer(std::move(reviewer).value()).has_value());
  REQUIRE(authored.set_executor(std::move(executor).value()).has_value());
  REQUIRE(authored.reviewer().has_value());
  REQUIRE(authored.executor().has_value());

  REQUIRE(
      authored.set_executor_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_value());
  REQUIRE(
      authored.set_reviewer_request_builder(wh::testing::helper::make_revision_request_builder())
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

TEST_CASE("reviewer executor accepts authored role providers directly",
          "[UT][wh/agent/reviewer_executor.hpp][reviewer_executor::set_reviewer][surface]"
          "[role_binding]") {
  wh::agent::reviewer_executor authored{"reviewer-executor-authored"};
  REQUIRE(authored.set_reviewer(wh::testing::helper::make_configured_chat("reviewer", "reviewer"))
              .has_value());
  REQUIRE(authored.set_executor(wh::testing::helper::make_configured_react("executor", "executor"))
              .has_value());
  REQUIRE(
      authored.set_executor_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_value());
  REQUIRE(
      authored.set_reviewer_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_value());
  REQUIRE(authored
              .set_review_decision_reader(wh::testing::helper::make_review_decision_reader(
                  wh::agent::review_decision_kind::accept))
              .has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lower().has_value());
}

TEST_CASE(
    "reviewer executor shell rejects missing duplicate non executable roles and late mutation",
    "[UT][wh/agent/"
    "reviewer_executor.hpp][reviewer_executor::set_executor_request_builder][condition][branch]["
    "boundary]") {
  wh::agent::reviewer_executor missing{"missing"};
  auto missing_freeze = missing.freeze();
  REQUIRE(missing_freeze.has_error());
  REQUIRE(missing_freeze.error() == wh::core::errc::invalid_argument);

  REQUIRE(missing.set_executor_request_builder(wh::agent::revision_request_builder{nullptr})
              .has_error());
  REQUIRE(missing.set_reviewer_request_builder(wh::agent::revision_request_builder{nullptr})
              .has_error());
  REQUIRE(
      missing.set_review_decision_reader(wh::agent::review_decision_reader{nullptr}).has_error());
  REQUIRE(missing.set_reviewer(wh::agent::agent{""}).has_error());
  REQUIRE(missing.set_executor(wh::agent::agent{""}).has_error());

  wh::agent::reviewer_executor duplicate{"duplicate"};
  REQUIRE(duplicate.set_reviewer(wh::agent::agent{"same"}).has_value());
  REQUIRE(duplicate.set_executor(wh::agent::agent{"same"}).has_value());
  REQUIRE(
      duplicate.set_executor_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_value());
  REQUIRE(
      duplicate.set_reviewer_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_value());
  REQUIRE(duplicate
              .set_review_decision_reader(wh::testing::helper::make_review_decision_reader(
                  wh::agent::review_decision_kind::accept))
              .has_value());
  auto duplicate_freeze = duplicate.freeze();
  REQUIRE(duplicate_freeze.has_error());
  REQUIRE(duplicate_freeze.error() == wh::core::errc::contract_violation);

  auto configured = wh::testing::helper::make_configured_reviewer_executor("configured");
  REQUIRE(configured.has_value());
  REQUIRE(configured->freeze().has_value());
  REQUIRE(configured->set_max_iterations(3U).has_error());
  REQUIRE(
      configured->set_executor_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_error());
  REQUIRE(configured->set_reviewer(wh::agent::agent{"late"}).has_error());
}
