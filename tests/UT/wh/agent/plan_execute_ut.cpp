#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/plan_execute.hpp"

TEST_CASE("plan execute contracts carry plan step context readers and effective replanner fallback",
          "[UT][wh/agent/plan_execute.hpp][plan_execute::freeze][condition][branch][boundary]") {
  wh::agent::plan_execute_plan plan{.steps = {"s1", "s2"}};
  wh::agent::plan_execute_executed_step executed{
      .step = "s1",
      .result = "done",
  };
  wh::agent::plan_execute_context context{
      .input_messages = {wh::testing::helper::make_text_message(
          wh::schema::message_role::user, "input")},
      .current_plan = plan,
      .executed_steps = {executed},
  };
  REQUIRE(context.current_plan->steps.size() == 2U);
  REQUIRE(context.executed_steps.front().result == "done");

  wh::agent::plan_execute_decision decision{
      .kind = wh::agent::plan_execute_decision_kind::respond,
      .response = wh::testing::helper::make_text_message(
          wh::schema::message_role::assistant, "final"),
  };
  REQUIRE(decision.kind == wh::agent::plan_execute_decision_kind::respond);

  wh::agent::plan_execute authored{"planner-shell"};
  REQUIRE(authored.name() == "planner-shell");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.max_iterations() == 8U);
  REQUIRE(authored.output_key().empty());
  REQUIRE(authored.planner().has_error());
  REQUIRE(authored.executor().has_error());
  REQUIRE(authored.replanner().has_error());
  REQUIRE(authored.effective_replanner_name().has_error());

  REQUIRE(authored.set_max_iterations(0U).has_value());
  REQUIRE(authored.max_iterations() == 1U);
  REQUIRE(authored.set_output_key("final").has_value());
  REQUIRE(authored.output_key() == "final");

  auto planner = wh::testing::helper::make_executable_agent("planner");
  auto executor = wh::testing::helper::make_executable_agent("executor");
  REQUIRE(planner.has_value());
  REQUIRE(executor.has_value());
  REQUIRE(authored.set_planner(std::move(planner).value()).has_value());
  REQUIRE(authored.set_executor(std::move(executor).value()).has_value());
  REQUIRE(authored.planner().has_value());
  REQUIRE(authored.executor().has_value());
  REQUIRE(authored.effective_replanner().has_value());
  REQUIRE(authored.effective_replanner_name().has_value());
  REQUIRE(authored.effective_replanner_name().value() == "planner");

  REQUIRE(authored
              .set_planner_request_builder(
                  wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(authored
              .set_executor_request_builder(
                  wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(authored
              .set_replanner_request_builder(
                  wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(authored.set_planner_plan_reader(
              wh::testing::helper::make_plan_reader())
              .has_value());
  REQUIRE(authored.set_executor_step_reader(
              wh::testing::helper::make_step_reader())
              .has_value());
  REQUIRE(authored.set_replanner_decision_reader(
              wh::testing::helper::make_plan_execute_decision_reader())
              .has_value());

  auto replanner = wh::testing::helper::make_executable_agent("replanner");
  REQUIRE(replanner.has_value());
  REQUIRE(authored.set_replanner(std::move(replanner).value()).has_value());
  REQUIRE(authored.replanner().has_value());
  REQUIRE(authored.effective_replanner_name().has_value());
  REQUIRE(authored.effective_replanner_name().value() == "replanner");
  REQUIRE(authored.freeze().has_value());
  REQUIRE(authored.frozen());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().executable());
}

TEST_CASE("plan execute shell rejects invalid builders roles and late mutation",
          "[UT][wh/agent/plan_execute.hpp][plan_execute::set_planner_request_builder][condition][branch][boundary]") {
  wh::agent::plan_execute missing{"missing"};
  auto missing_freeze = missing.freeze();
  REQUIRE(missing_freeze.has_error());
  REQUIRE(missing_freeze.error() == wh::core::errc::invalid_argument);

  REQUIRE(missing
              .set_planner_request_builder(
                  wh::agent::plan_execute_request_builder{nullptr})
              .has_error());
  REQUIRE(missing
              .set_executor_request_builder(
                  wh::agent::plan_execute_request_builder{nullptr})
              .has_error());
  REQUIRE(missing
              .set_replanner_request_builder(
                  wh::agent::plan_execute_request_builder{nullptr})
              .has_error());
  REQUIRE(missing
              .set_planner_plan_reader(
                  wh::agent::plan_execute_plan_reader{nullptr})
              .has_error());
  REQUIRE(missing
              .set_executor_step_reader(
                  wh::agent::plan_execute_step_reader{nullptr})
              .has_error());
  REQUIRE(missing
              .set_replanner_decision_reader(
                  wh::agent::plan_execute_decision_reader{nullptr})
              .has_error());
  REQUIRE(missing.set_planner(wh::agent::agent{""}).has_error());
  REQUIRE(missing.set_executor(wh::agent::agent{""}).has_error());
  REQUIRE(missing.set_replanner(wh::agent::agent{""}).has_error());

  wh::agent::plan_execute conflicting{"conflicting"};
  REQUIRE(conflicting.set_planner(wh::agent::agent{"same"}).has_value());
  REQUIRE(conflicting.set_executor(wh::agent::agent{"same"}).has_value());
  REQUIRE(conflicting
              .set_planner_request_builder(
                  wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(conflicting
              .set_executor_request_builder(
                  wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(conflicting
              .set_replanner_request_builder(
                  wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(conflicting.set_planner_plan_reader(
              wh::testing::helper::make_plan_reader())
              .has_value());
  REQUIRE(conflicting.set_executor_step_reader(
              wh::testing::helper::make_step_reader())
              .has_value());
  REQUIRE(conflicting.set_replanner_decision_reader(
              wh::testing::helper::make_plan_execute_decision_reader())
              .has_value());
  auto conflicting_freeze = conflicting.freeze();
  REQUIRE(conflicting_freeze.has_error());
  REQUIRE(conflicting_freeze.error() == wh::core::errc::contract_violation);

  auto configured =
      wh::testing::helper::make_configured_plan_execute("configured");
  REQUIRE(configured.has_value());
  REQUIRE(configured->freeze().has_value());
  REQUIRE(configured->set_output_key("late").has_error());
  REQUIRE(configured->set_planner_request_builder(
              wh::testing::helper::make_plan_request_builder())
              .has_error());
  REQUIRE(configured->set_planner(wh::agent::agent{"late"}).has_error());
  REQUIRE(configured->set_max_iterations(3U).has_error());
}
