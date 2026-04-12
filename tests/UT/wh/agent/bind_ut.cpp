#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"

namespace {

auto expect_bound_agent(const wh::core::result<wh::agent::agent> &bound,
                        const std::string_view name) -> void {
  REQUIRE(bound.has_value());
  REQUIRE(bound->name() == name);
  REQUIRE(bound->executable());
}

} // namespace

TEST_CASE("agent bind make_agent lowers chat and react authored shells",
          "[UT][wh/agent/bind.hpp][make_agent(chat)][condition][branch][boundary]") {
  expect_bound_agent(
      wh::agent::make_agent(
          wh::testing::helper::make_configured_chat("chat", "desc")),
      "chat");
  expect_bound_agent(
      wh::agent::make_agent(
          wh::testing::helper::make_configured_react("react", "desc")),
      "react");
}

TEST_CASE("agent bind make_agent lowers plan-execute self-refine and reviewer-executor shells",
          "[UT][wh/agent/bind.hpp][make_agent(plan_execute)][condition][branch][boundary]") {
  auto plan = wh::testing::helper::make_configured_plan_execute("plan");
  REQUIRE(plan.has_value());
  expect_bound_agent(wh::agent::make_agent(std::move(plan).value()), "plan");

  auto self_refine =
      wh::testing::helper::make_configured_self_refine("self-refine");
  REQUIRE(self_refine.has_value());
  expect_bound_agent(wh::agent::make_agent(std::move(self_refine).value()),
                     "self-refine");

  auto reviewer =
      wh::testing::helper::make_configured_reviewer_executor("reviewer");
  REQUIRE(reviewer.has_value());
  expect_bound_agent(wh::agent::make_agent(std::move(reviewer).value()),
                     "reviewer");
}

TEST_CASE("agent bind make_agent lowers reflexion supervisor swarm and research shells",
          "[UT][wh/agent/bind.hpp][make_agent(reflexion)][condition][branch][boundary]") {
  auto reflexion = wh::testing::helper::make_configured_reflexion("reflexion");
  REQUIRE(reflexion.has_value());
  expect_bound_agent(wh::agent::make_agent(std::move(reflexion).value()),
                     "reflexion");

  auto supervisor =
      wh::testing::helper::make_configured_supervisor("supervisor");
  REQUIRE(supervisor.has_value());
  expect_bound_agent(wh::agent::make_agent(std::move(supervisor).value()),
                     "supervisor");

  auto swarm = wh::testing::helper::make_configured_swarm("swarm");
  REQUIRE(swarm.has_value());
  expect_bound_agent(wh::agent::make_agent(std::move(swarm).value()), "swarm");

  auto research = wh::testing::helper::make_configured_research("research");
  REQUIRE(research.has_value());
  expect_bound_agent(wh::agent::make_agent(std::move(research).value()),
                     "research");
}
