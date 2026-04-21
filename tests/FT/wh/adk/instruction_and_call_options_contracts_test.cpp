#include <chrono>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/adk/call_options.hpp"
#include "wh/agent/instruction.hpp"

TEST_CASE("adk instruction append replace and priority order are stable",
          "[core][adk][condition]") {
  wh::agent::instruction instruction{};
  instruction.append("system", 0);
  instruction.append("agent", 5);
  REQUIRE(instruction.render() == "system\nagent");

  instruction.replace("call", 10);
  instruction.append("ignored", 9);
  instruction.append("tail", 11);
  REQUIRE(instruction.render() == "call\ntail");
}

TEST_CASE("adk call options overlay scope filtering and impl specific mapping are stable",
          "[core][adk][condition]") {
  wh::adk::call_options defaults{};
  REQUIRE(wh::adk::set_global_option(defaults, "temperature", 0.1).has_value());
  defaults.budget.max_concurrency = 2U;

  wh::adk::call_options flow{};
  REQUIRE(wh::adk::set_agent_option(flow, "planner", "mode", std::string{"plan"}).has_value());
  REQUIRE(wh::adk::set_impl_option(flow, "openai", "model", std::string{"gpt-5"}).has_value());

  wh::adk::call_options adk{};
  REQUIRE(wh::adk::set_global_option(adk, "temperature", 0.2).has_value());
  REQUIRE(wh::adk::set_checkpoint_field(adk, "resume-id", std::string{"resume-1"}).has_value());
  adk.transfer_trim.trim_assistant_transfer_message = true;
  adk.budget.max_concurrency = 4U;

  wh::adk::call_options call_override{};
  REQUIRE(
      wh::adk::set_tool_option(call_override, "search", "timeout", std::chrono::milliseconds{250})
          .has_value());
  call_override.transfer_trim.trim_tool_transfer_pair = true;
  call_override.budget.max_iterations = 8U;

  const auto merged = wh::adk::resolve_call_options(&defaults, &flow, &adk, &call_override);

  const auto planner = wh::adk::materialize_agent_scope(merged, "planner");
  REQUIRE(wh::adk::option_value_copy<double>(planner.values, "temperature").value() == 0.2);
  REQUIRE(wh::adk::option_value_copy<std::string>(planner.values, "mode").value() == "plan");
  REQUIRE(wh::adk::option_value_copy<std::string>(planner.checkpoint_fields, "resume-id").value() ==
          "resume-1");
  REQUIRE(planner.transfer_trim.trim_assistant_transfer_message);
  REQUIRE(planner.transfer_trim.trim_tool_transfer_pair);
  REQUIRE(planner.budget.max_concurrency == 4U);
  REQUIRE(planner.budget.max_iterations == 8U);
  REQUIRE(planner.impl_specific.contains("openai"));
  REQUIRE(wh::adk::option_value_copy<std::string>(planner.impl_specific.at("openai"), "model")
              .value() == "gpt-5");

  const auto worker = wh::adk::materialize_agent_scope(merged, "worker");
  REQUIRE(wh::adk::option_value_copy<std::string>(worker.values, "mode").has_error());
  REQUIRE(wh::adk::option_value_copy<std::string>(worker.values, "mode").error() ==
          wh::core::errc::not_found);

  const auto search = wh::adk::materialize_tool_scope(merged, "search");
  REQUIRE(wh::adk::option_value_copy<std::chrono::milliseconds>(search.values, "timeout").value() ==
          std::chrono::milliseconds{250});
  REQUIRE(wh::adk::option_value_copy<std::string>(search.values, "temperature").has_error());
  REQUIRE(wh::adk::option_value_copy<std::string>(search.values, "temperature").error() ==
          wh::core::errc::type_mismatch);
}
