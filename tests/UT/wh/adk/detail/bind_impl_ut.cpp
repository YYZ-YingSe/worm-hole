#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/detail/bind_impl.hpp"

TEST_CASE("adk bind impl umbrella exposes concrete detail binders for all authored shell families",
          "[UT][wh/adk/detail/bind_impl.hpp][bind_chat_agent][condition][branch][boundary]") {
  auto chat_authored = wh::testing::helper::make_configured_chat("chat", "desc");
  REQUIRE(chat_authored.freeze().has_value());
  auto chat = wh::adk::detail::bind_chat_agent(std::move(chat_authored));
  REQUIRE(chat.has_value());
  REQUIRE(chat.value().name() == "chat");

  auto react_authored = wh::testing::helper::make_configured_react("react", "desc");
  REQUIRE(react_authored.freeze().has_value());
  auto react = wh::adk::detail::bind_react_agent(std::move(react_authored));
  REQUIRE(react.has_value());
  REQUIRE(react.value().name() == "react");

  auto plan = wh::testing::helper::make_configured_plan_execute("plan");
  REQUIRE(plan.has_value());
  REQUIRE(plan->freeze().has_value());
  REQUIRE(wh::adk::detail::bind_plan_execute_agent(std::move(plan).value()).has_value());

  auto self_refine = wh::testing::helper::make_configured_self_refine("self-refine");
  REQUIRE(self_refine.has_value());
  REQUIRE(self_refine->freeze().has_value());
  REQUIRE(wh::adk::detail::bind_self_refine_agent(std::move(self_refine).value()).has_value());

  auto reviewer = wh::testing::helper::make_configured_reviewer_executor("reviewer");
  REQUIRE(reviewer.has_value());
  REQUIRE(reviewer->freeze().has_value());
  REQUIRE(wh::adk::detail::bind_reviewer_executor_agent(std::move(reviewer).value()).has_value());

  auto reflexion = wh::testing::helper::make_configured_reflexion("reflexion");
  REQUIRE(reflexion.has_value());
  REQUIRE(reflexion->freeze().has_value());
  REQUIRE(wh::adk::detail::bind_reflexion_agent(std::move(reflexion).value()).has_value());

  auto supervisor = wh::testing::helper::make_configured_supervisor("supervisor");
  REQUIRE(supervisor.has_value());
  REQUIRE(supervisor->freeze().has_value());
  REQUIRE(wh::adk::detail::bind_supervisor_agent(std::move(supervisor).value()).has_value());

  auto swarm = wh::testing::helper::make_configured_swarm("swarm");
  REQUIRE(swarm.has_value());
  REQUIRE(swarm->freeze().has_value());
  REQUIRE(wh::adk::detail::bind_swarm_agent(std::move(swarm).value()).has_value());

  auto research = wh::testing::helper::make_configured_research("research");
  REQUIRE(research.has_value());
  REQUIRE(research->freeze().has_value());
  REQUIRE(wh::adk::detail::bind_research_agent(std::move(research).value()).has_value());
}

TEST_CASE("adk bind impl lowerings preserve authored names and descriptions after moves",
          "[UT][wh/adk/detail/bind_impl.hpp][bind_react_agent][condition][branch][boundary]") {
  auto chat = wh::testing::helper::make_configured_chat("chat-move", "chat-desc");
  REQUIRE(chat.freeze().has_value());
  auto bound_chat = wh::adk::detail::bind_chat_agent(std::move(chat));
  REQUIRE(bound_chat.has_value());
  REQUIRE(bound_chat.value().name() == "chat-move");
  REQUIRE(bound_chat.value().description() == "chat-desc");

  auto react = wh::testing::helper::make_configured_react("react-move", "react-desc");
  REQUIRE(react.freeze().has_value());
  auto bound_react = wh::adk::detail::bind_react_agent(std::move(react));
  REQUIRE(bound_react.has_value());
  REQUIRE(bound_react.value().name() == "react-move");
  REQUIRE(bound_react.value().description() == "react-desc");
}
