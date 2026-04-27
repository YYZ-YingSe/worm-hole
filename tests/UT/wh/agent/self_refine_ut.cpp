#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/self_refine.hpp"

TEST_CASE("self refine shell binds worker optional reviewer and lowers into executable agent",
          "[UT][wh/agent/self_refine.hpp][self_refine::freeze][condition][branch][boundary]") {
  wh::agent::self_refine authored{"self-refine"};
  REQUIRE(authored.name() == "self-refine");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.max_iterations() == 3U);
  REQUIRE(authored.worker().has_error());
  REQUIRE(authored.reviewer().has_error());
  REQUIRE(authored.effective_reviewer().has_error());

  REQUIRE(authored.set_max_iterations(0U).has_value());
  REQUIRE(authored.max_iterations() == 1U);

  auto worker = wh::testing::helper::make_executable_agent("worker");
  auto reviewer = wh::testing::helper::make_executable_agent("reviewer");
  REQUIRE(worker.has_value());
  REQUIRE(reviewer.has_value());
  REQUIRE(authored.set_worker(std::move(worker).value()).has_value());
  REQUIRE(authored.worker().has_value());
  REQUIRE(authored.effective_reviewer().has_value());
  REQUIRE(authored.effective_reviewer().value().get().name() == "worker");
  REQUIRE(authored.set_reviewer(std::move(reviewer).value()).has_value());
  REQUIRE(authored.reviewer().has_value());
  REQUIRE(authored.effective_reviewer().value().get().name() == "reviewer");

  REQUIRE(authored.set_worker_request_builder(wh::testing::helper::make_revision_request_builder())
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

TEST_CASE("self refine accepts authored role providers directly",
          "[UT][wh/agent/self_refine.hpp][self_refine::set_worker][surface][role_binding]") {
  wh::agent::self_refine authored{"self-refine-authored"};
  REQUIRE(authored.set_worker(wh::testing::helper::make_configured_chat("worker", "worker"))
              .has_value());
  REQUIRE(authored.set_reviewer(wh::testing::helper::make_configured_react("reviewer", "reviewer"))
              .has_value());
  REQUIRE(authored.set_worker_request_builder(wh::testing::helper::make_revision_request_builder())
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
    "self refine shell rejects missing builders non executable worker and late mutation",
    "[UT][wh/agent/"
    "self_refine.hpp][self_refine::set_worker_request_builder][condition][branch][boundary]") {
  wh::agent::self_refine missing{"missing"};
  auto missing_freeze = missing.freeze();
  REQUIRE(missing_freeze.has_error());
  REQUIRE(missing_freeze.error() == wh::core::errc::invalid_argument);

  REQUIRE(
      missing.set_worker_request_builder(wh::agent::revision_request_builder{nullptr}).has_error());
  REQUIRE(missing.set_reviewer_request_builder(wh::agent::revision_request_builder{nullptr})
              .has_error());
  REQUIRE(
      missing.set_review_decision_reader(wh::agent::review_decision_reader{nullptr}).has_error());
  REQUIRE(missing.set_worker(wh::agent::agent{""}).has_error());
  REQUIRE(missing.set_reviewer(wh::agent::agent{""}).has_error());

  wh::agent::self_refine non_executable{"non-executable"};
  REQUIRE(non_executable.set_worker(wh::agent::agent{"worker"}).has_value());
  REQUIRE(non_executable
              .set_worker_request_builder(wh::testing::helper::make_revision_request_builder())
              .has_value());
  REQUIRE(non_executable
              .set_reviewer_request_builder(wh::testing::helper::make_revision_request_builder())
              .has_value());
  REQUIRE(non_executable
              .set_review_decision_reader(wh::testing::helper::make_review_decision_reader(
                  wh::agent::review_decision_kind::accept))
              .has_value());
  auto non_executable_freeze = non_executable.freeze();
  REQUIRE(non_executable_freeze.has_error());
  REQUIRE(non_executable_freeze.error() == wh::core::errc::contract_violation);

  auto configured = wh::testing::helper::make_configured_self_refine("configured");
  REQUIRE(configured.has_value());
  REQUIRE(configured->freeze().has_value());
  REQUIRE(configured->set_max_iterations(3U).has_error());
  REQUIRE(
      configured->set_worker_request_builder(wh::testing::helper::make_revision_request_builder())
          .has_error());
  REQUIRE(configured->set_worker(wh::agent::agent{"late"}).has_error());
}
