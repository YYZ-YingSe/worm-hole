#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "wh/adk/call_options.hpp"

static_assert(std::same_as<wh::adk::option_string_hash,
                           wh::core::transparent_string_hash>);
static_assert(std::same_as<wh::adk::option_string_equal,
                           wh::core::transparent_string_equal>);

TEST_CASE("adk call options set_option and option_value_copy cover hit miss and type mismatch",
          "[UT][wh/adk/call_options.hpp][option_value_copy][condition][branch][boundary]") {
  wh::adk::option_bag bag{};
  REQUIRE(wh::adk::set_option(bag, "answer", 42).has_value());
  REQUIRE(wh::adk::set_option(bag, "raw", wh::core::any{std::string{"ok"}}).has_value());

  REQUIRE(wh::adk::option_value_copy<int>(bag, "answer").value() == 42);
  REQUIRE(wh::adk::option_value_copy<std::string>(bag, "raw").value() == "ok");
  REQUIRE(wh::adk::option_value_copy<int>(bag, "missing").error() ==
          wh::core::errc::not_found);
  REQUIRE(wh::adk::option_value_copy<std::string>(bag, "answer").error() ==
          wh::core::errc::type_mismatch);
}

TEST_CASE("adk call options named scope helpers upsert find and merge deterministic overlays",
          "[UT][wh/adk/call_options.hpp][upsert_scope][condition][branch][boundary]") {
  std::vector<wh::adk::named_option_bag> scopes{};

  auto &planner = wh::adk::detail::upsert_scope(scopes, "planner");
  REQUIRE(wh::adk::set_option(planner.values, "mode", std::string{"plan"}).has_value());
  auto &same_planner = wh::adk::detail::upsert_scope(scopes, "planner");

  REQUIRE(&same_planner == &planner);
  REQUIRE(wh::adk::detail::find_named_scope(scopes, "planner") == &planner);
  REQUIRE(wh::adk::detail::find_named_scope(scopes, "worker") == nullptr);

  std::vector<wh::adk::named_option_bag> next_scopes{};
  wh::adk::detail::upsert_scope(next_scopes, "planner");
  wh::adk::detail::upsert_scope(next_scopes, "worker");
  REQUIRE(wh::adk::set_option(
      wh::adk::detail::find_named_scope(next_scopes, "planner")->values, "mode",
      std::string{"override"}).has_value());
  REQUIRE(wh::adk::set_option(
      wh::adk::detail::find_named_scope(next_scopes, "worker")->values, "stage",
      std::string{"execute"}).has_value());

  wh::adk::detail::merge_named_scopes(scopes, next_scopes);

  const auto *merged_planner = wh::adk::detail::find_named_scope(scopes, "planner");
  REQUIRE(merged_planner != nullptr);
  REQUIRE(wh::adk::option_value_copy<std::string>(merged_planner->values, "mode")
              .value() == "override");
  REQUIRE(scopes.size() == 2U);
  REQUIRE(wh::adk::option_value_copy<std::string>(scopes[1].values, "stage").value() ==
          "execute");
}

TEST_CASE("adk call options overlay helpers update only provided budget trim and impl fields",
          "[UT][wh/adk/call_options.hpp][overlay_budget][condition][branch][boundary]") {
  wh::adk::call_budget_options budget{};
  wh::adk::detail::overlay_budget(
      budget, {.max_concurrency = 2U, .fail_fast = true});
  wh::adk::detail::overlay_budget(
      budget, {.max_iterations = 8U, .timeout = std::chrono::milliseconds{250}});
  REQUIRE(budget.max_concurrency == std::optional<std::size_t>{2U});
  REQUIRE(budget.max_iterations == std::optional<std::size_t>{8U});
  REQUIRE(budget.timeout == std::optional<std::chrono::milliseconds>{
                                std::chrono::milliseconds{250}});
  REQUIRE(budget.fail_fast == std::optional<bool>{true});

  wh::adk::transfer_trim_options trim{};
  wh::adk::detail::overlay_transfer_trim(
      trim, {.trim_assistant_transfer_message = true});
  wh::adk::detail::overlay_transfer_trim(
      trim, {.trim_tool_transfer_pair = true});
  const auto resolved_trim = wh::adk::detail::materialize_resolved_trim(trim);
  REQUIRE(resolved_trim.trim_assistant_transfer_message);
  REQUIRE(resolved_trim.trim_tool_transfer_pair);

  std::unordered_map<std::string, wh::adk::option_bag, wh::adk::option_string_hash,
                     wh::adk::option_string_equal>
      target{};
  std::unordered_map<std::string, wh::adk::option_bag, wh::adk::option_string_hash,
                     wh::adk::option_string_equal>
      next{};
  REQUIRE(wh::adk::set_option(target["openai"], "model", std::string{"gpt-4"}).has_value());
  REQUIRE(wh::adk::set_option(next["openai"], "model", std::string{"gpt-5"}).has_value());
  REQUIRE(wh::adk::set_option(next["anthropic"], "model", std::string{"claude"}).has_value());
  wh::adk::detail::merge_impl_specific(target, next);
  REQUIRE(wh::adk::option_value_copy<std::string>(target["openai"], "model").value() ==
          "gpt-5");
  REQUIRE(wh::adk::option_value_copy<std::string>(target["anthropic"], "model")
              .value() == "claude");
}

TEST_CASE("adk call options resolve_call_options overlays defaults workflow adk and call override",
          "[UT][wh/adk/call_options.hpp][resolve_call_options][condition][branch][boundary]") {
  wh::adk::call_options defaults{};
  REQUIRE(wh::adk::set_global_option(defaults, "temperature", 0.1).has_value());
  defaults.budget.max_concurrency = 2U;

  wh::adk::call_options workflow{};
  REQUIRE(wh::adk::set_agent_option(workflow, "planner", "mode", std::string{"plan"}).has_value());
  REQUIRE(wh::adk::set_impl_option(workflow, "openai", "model", std::string{"gpt-5"}).has_value());

  wh::adk::call_options adk{};
  REQUIRE(wh::adk::set_global_option(adk, "temperature", 0.2).has_value());
  REQUIRE(wh::adk::set_checkpoint_field(adk, "resume-id", std::string{"resume-1"}).has_value());
  adk.transfer_trim.trim_assistant_transfer_message = true;
  adk.budget.max_concurrency = 4U;

  wh::adk::call_options call_override{};
  REQUIRE(wh::adk::set_tool_option(call_override, "search", "timeout",
                                   std::chrono::milliseconds{250})
              .has_value());
  call_override.transfer_trim.trim_tool_transfer_pair = true;
  call_override.budget.max_iterations = 8U;

  const auto merged =
      wh::adk::resolve_call_options(&defaults, &workflow, &adk, &call_override);

  REQUIRE(wh::adk::option_value_copy<double>(merged.global, "temperature").value() ==
          0.2);
  REQUIRE(merged.agent_scopes.size() == 1U);
  REQUIRE(merged.tool_scopes.size() == 1U);
  REQUIRE(merged.budget.max_concurrency == std::optional<std::size_t>{4U});
  REQUIRE(merged.budget.max_iterations == std::optional<std::size_t>{8U});
}

TEST_CASE("adk call options materialize agent and tool scopes with correct visibility",
          "[UT][wh/adk/call_options.hpp][materialize_tool_scope][condition][branch][boundary]") {
  wh::adk::call_options merged{};
  REQUIRE(wh::adk::set_global_option(merged, "temperature", 0.2).has_value());
  REQUIRE(wh::adk::set_agent_option(merged, "planner", "mode", std::string{"plan"}).has_value());
  REQUIRE(wh::adk::set_tool_option(merged, "search", "timeout",
                                   std::chrono::milliseconds{250})
              .has_value());
  REQUIRE(wh::adk::set_checkpoint_field(merged, "resume-id", std::string{"resume-1"}).has_value());
  REQUIRE(wh::adk::set_impl_option(merged, "openai", "model", std::string{"gpt-5"}).has_value());
  merged.transfer_trim.trim_assistant_transfer_message = true;
  merged.transfer_trim.trim_tool_transfer_pair = true;
  merged.budget.max_concurrency = 4U;
  merged.budget.max_iterations = 8U;

  const auto planner_scope = wh::adk::materialize_agent_scope(merged, "planner");
  REQUIRE(wh::adk::option_value_copy<double>(planner_scope.values, "temperature")
              .value() == 0.2);
  REQUIRE(wh::adk::option_value_copy<std::string>(planner_scope.values, "mode")
              .value() == "plan");
  REQUIRE(wh::adk::option_value_copy<std::chrono::milliseconds>(
              planner_scope.values, "timeout")
              .error() == wh::core::errc::not_found);
  REQUIRE(wh::adk::option_value_copy<std::string>(
              planner_scope.checkpoint_fields, "resume-id")
              .value() == "resume-1");
  REQUIRE(planner_scope.transfer_trim.trim_assistant_transfer_message);
  REQUIRE(planner_scope.transfer_trim.trim_tool_transfer_pair);
  REQUIRE(planner_scope.impl_specific.contains("openai"));

  const auto search_scope = wh::adk::materialize_tool_scope(merged, "search");
  REQUIRE(wh::adk::option_value_copy<std::chrono::milliseconds>(
              search_scope.values, "timeout")
              .value() == std::chrono::milliseconds{250});
  REQUIRE(wh::adk::option_value_copy<std::string>(search_scope.values, "temperature")
              .error() == wh::core::errc::type_mismatch);
  REQUIRE(wh::adk::option_value_copy<std::string>(search_scope.values, "mode")
              .error() == wh::core::errc::not_found);
}
