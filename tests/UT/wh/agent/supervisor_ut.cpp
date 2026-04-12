#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/supervisor.hpp"

TEST_CASE("supervisor shell binds supervisor workers validates names and lowers into executable agent",
          "[UT][wh/agent/supervisor.hpp][supervisor::freeze][condition][branch][boundary]") {
  wh::agent::supervisor authored{"lead"};
  REQUIRE(authored.name() == "lead");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.supervisor_agent().has_error());
  REQUIRE(authored.workers().empty());

  auto supervisor = wh::testing::helper::make_executable_agent("lead");
  auto worker = wh::testing::helper::make_executable_agent("worker");
  REQUIRE(supervisor.has_value());
  REQUIRE(worker.has_value());
  REQUIRE(authored.set_supervisor(std::move(supervisor).value()).has_value());
  REQUIRE(authored.add_worker(std::move(worker).value()).has_value());
  REQUIRE(authored.supervisor_agent().has_value());
  REQUIRE(authored.worker_names() == std::vector<std::string>{"worker"});
  REQUIRE(authored.workers().size() == 1U);
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().executable());
}

TEST_CASE("supervisor shell rejects invalid role topology duplicates and late mutation",
          "[UT][wh/agent/supervisor.hpp][supervisor::add_worker][condition][branch][boundary]") {
  wh::agent::supervisor missing{"lead"};
  auto missing_freeze = missing.freeze();
  REQUIRE(missing_freeze.has_error());
  REQUIRE(missing_freeze.error() == wh::core::errc::invalid_argument);
  REQUIRE(missing.set_supervisor(wh::agent::agent{""}).has_error());
  REQUIRE(missing.add_worker(wh::agent::agent{""}).has_error());

  wh::agent::supervisor duplicate{"lead"};
  auto supervisor = wh::testing::helper::make_executable_agent("lead");
  auto worker_a = wh::testing::helper::make_executable_agent("dup");
  auto worker_b = wh::testing::helper::make_executable_agent("dup");
  REQUIRE(supervisor.has_value());
  REQUIRE(worker_a.has_value());
  REQUIRE(worker_b.has_value());
  REQUIRE(duplicate.set_supervisor(std::move(supervisor).value()).has_value());
  REQUIRE(duplicate.add_worker(std::move(worker_a).value()).has_value());
  REQUIRE(duplicate.add_worker(std::move(worker_b).value()).has_value());
  auto duplicate_freeze = duplicate.freeze();
  REQUIRE(duplicate_freeze.has_error());
  REQUIRE(duplicate_freeze.error() == wh::core::errc::already_exists);

  auto configured =
      wh::testing::helper::make_configured_supervisor("configured");
  REQUIRE(configured.has_value());
  REQUIRE(configured->freeze().has_value());
  REQUIRE(configured->set_supervisor(wh::agent::agent{"late"}).has_error());
  REQUIRE(configured->add_worker(wh::agent::agent{"late"}).has_error());
}
