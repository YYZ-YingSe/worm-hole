#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/bind.hpp"

namespace {

auto expect_bound_agent(const wh::core::result<wh::agent::agent> &bound,
                        const std::string_view name) -> void {
  REQUIRE(bound.has_value());
  REQUIRE(bound->name() == name);
  REQUIRE(bound->executable());
}

} // namespace

TEST_CASE("adk bind facade lowers chat and react authored shells",
          "[UT][wh/adk/bind.hpp][bind_chat_agent][condition][branch][boundary]") {
  auto chat = wh::testing::helper::make_configured_chat("chat", "desc");
  REQUIRE(chat.freeze().has_value());
  expect_bound_agent(wh::adk::bind_chat_agent(std::move(chat)), "chat");

  auto react = wh::testing::helper::make_configured_react("react", "desc");
  REQUIRE(react.freeze().has_value());
  expect_bound_agent(wh::adk::bind_react_agent(std::move(react)), "react");
}

TEST_CASE("adk bind facade lowers plan-execute self-refine and reviewer-executor shells",
          "[UT][wh/adk/bind.hpp][bind_plan_execute_agent][condition][branch][boundary]") {
  auto plan = wh::testing::helper::make_configured_plan_execute("plan");
  REQUIRE(plan.has_value());
  REQUIRE(plan->freeze().has_value());
  expect_bound_agent(wh::adk::bind_plan_execute_agent(std::move(plan).value()), "plan");

  auto self_refine = wh::testing::helper::make_configured_self_refine("self-refine");
  REQUIRE(self_refine.has_value());
  REQUIRE(self_refine->freeze().has_value());
  expect_bound_agent(wh::adk::bind_self_refine_agent(std::move(self_refine).value()),
                     "self-refine");

  auto reviewer = wh::testing::helper::make_configured_reviewer_executor("reviewer");
  REQUIRE(reviewer.has_value());
  REQUIRE(reviewer->freeze().has_value());
  expect_bound_agent(wh::adk::bind_reviewer_executor_agent(std::move(reviewer).value()),
                     "reviewer");
}

TEST_CASE("adk bind facade lowers reflexion supervisor swarm and research shells",
          "[UT][wh/adk/bind.hpp][bind_reflexion_agent][condition][branch][boundary]") {
  auto reflexion = wh::testing::helper::make_configured_reflexion("reflexion");
  REQUIRE(reflexion.has_value());
  REQUIRE(reflexion->freeze().has_value());
  expect_bound_agent(wh::adk::bind_reflexion_agent(std::move(reflexion).value()), "reflexion");

  auto supervisor = wh::testing::helper::make_configured_supervisor("supervisor");
  REQUIRE(supervisor.has_value());
  REQUIRE(supervisor->freeze().has_value());
  expect_bound_agent(wh::adk::bind_supervisor_agent(std::move(supervisor).value()), "supervisor");

  auto swarm = wh::testing::helper::make_configured_swarm("swarm");
  REQUIRE(swarm.has_value());
  REQUIRE(swarm->freeze().has_value());
  expect_bound_agent(wh::adk::bind_swarm_agent(std::move(swarm).value()), "swarm");

  auto research = wh::testing::helper::make_configured_research("research");
  REQUIRE(research.has_value());
  REQUIRE(research->freeze().has_value());
  expect_bound_agent(wh::adk::bind_research_agent(std::move(research).value()), "research");
}
