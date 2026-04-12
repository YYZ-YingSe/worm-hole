#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "wh/compose/authored/workflow.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("workflow exposes stable input keys and step handles",
          "[UT][wh/compose/authored/workflow.hpp][workflow_input_node_key][condition][branch][boundary]") {
  REQUIRE(wh::compose::workflow_input_node_key_prefix == "__wf_input__");
  REQUIRE(wh::compose::workflow_input_node_key("node") == "__wf_input__node");
  STATIC_REQUIRE(!std::is_default_constructible_v<wh::compose::workflow::step_ref>);

  wh::compose::workflow workflow{};

  auto step = workflow.add_step(wh::compose::make_passthrough_node("first"));
  REQUIRE(step.has_value());
  REQUIRE(step.value().key() == "first");
  REQUIRE(workflow.step("first").has_value());
  REQUIRE(workflow.step("missing").has_error());
}

TEST_CASE("workflow compiles invokes and maps entry/static values",
          "[UT][wh/compose/authored/workflow.hpp][workflow::invoke][condition][branch][boundary]") {
  wh::compose::workflow workflow{};
  auto step = workflow.add_step(wh::compose::make_passthrough_node("target"));
  REQUIRE(step.has_value());

  wh::compose::field_mapping_rule mapping{};
  mapping.from_path = "input.id";
  mapping.to_path = "request.user_id";
  REQUIRE(step.value().from_entry(std::vector{mapping}).has_value());
  REQUIRE(step.value()
              .set_static_value("request.source", std::string{"ut"})
              .has_value());
  REQUIRE(workflow.end().add_input(step.value()).has_value());
  REQUIRE(workflow.compile().has_value());
  REQUIRE(workflow.graph_view().compiled());

  wh::compose::graph_value_map input{};
  input.insert_or_assign(
      "input",
      wh::compose::graph_value{wh::compose::graph_value_map{
          {"id", wh::compose::graph_value{7}},
      }});
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(workflow.invoke(context, input));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  auto *request =
      wh::core::any_cast<wh::compose::graph_value_map>(&std::get<0>(*awaited).value().at("request"));
  REQUIRE(request != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&request->at("user_id")) == 7);
  REQUIRE(*wh::core::any_cast<std::string>(&request->at("source")) == "ut");
}

TEST_CASE("workflow validates dependency staging compile freeze and data edges",
          "[UT][wh/compose/authored/workflow.hpp][workflow::compile][condition][branch][boundary]") {
  wh::compose::workflow empty{};
  auto empty_compile = empty.compile();
  REQUIRE(empty_compile.has_error());
  REQUIRE(empty_compile.error() == wh::core::errc::contract_violation);

  wh::compose::workflow invalid_workflow{};
  auto invalid_first =
      invalid_workflow.add_step(wh::compose::make_passthrough_node("first"));
  auto invalid_second =
      invalid_workflow.add_step(wh::compose::make_passthrough_node("second"));
  REQUIRE(invalid_first.has_value());
  REQUIRE(invalid_second.has_value());
  REQUIRE(
      invalid_second.value().add_input_without_control(invalid_first.value())
          .has_error());

  wh::compose::workflow workflow{};
  auto first = workflow.add_step(wh::compose::make_passthrough_node("first"));
  auto second = workflow.add_step(wh::compose::make_passthrough_node("second"));
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());

  REQUIRE(first.value().from_entry().has_value());
  REQUIRE(second.value().add_dependency(first.value()).has_value());
  REQUIRE(workflow.end().add_input(second.value()).has_value());
  REQUIRE(workflow.compile().has_value());

  wh::compose::workflow data_workflow{};
  auto data_first =
      data_workflow.add_step(wh::compose::make_passthrough_node("first"));
  auto data_second =
      data_workflow.add_step(wh::compose::make_passthrough_node("second"));
  REQUIRE(data_first.has_value());
  REQUIRE(data_second.has_value());
  REQUIRE(data_first.value().from_entry().has_value());
  REQUIRE(data_second.value().add_input(data_first.value()).has_value());
  REQUIRE(data_workflow.end().add_input(data_second.value()).has_value());
  REQUIRE(data_workflow.compile().has_value());

  REQUIRE(
      data_workflow.add_step(wh::compose::make_passthrough_node("late"))
          .has_error());
  REQUIRE(data_workflow.step("first").has_error());
}

TEST_CASE("workflow rejects step handles borrowed from another workflow",
          "[UT][wh/compose/authored/workflow.hpp][workflow::step_ref::add_input][condition][branch][boundary]") {
  wh::compose::workflow left{};
  wh::compose::workflow right{};
  auto left_source = left.add_step(wh::compose::make_passthrough_node("source"));
  auto right_target = right.add_step(wh::compose::make_passthrough_node("target"));
  REQUIRE(left_source.has_value());
  REQUIRE(right_target.has_value());

  REQUIRE(right_target.value().add_input(left_source.value()).has_error());
  REQUIRE(right_target.value().add_dependency(left_source.value()).has_error());
  REQUIRE(right_target.value()
              .add_input_without_control(left_source.value())
              .has_error());
}

TEST_CASE("workflow step_ref accepts entry-only control dependencies and static value staging before compile",
          "[UT][wh/compose/authored/workflow.hpp][workflow::step_ref::from_entry_without_input][condition][branch][boundary]") {
  wh::compose::workflow workflow{};
  auto step = workflow.add_step(wh::compose::make_passthrough_node("target"));
  REQUIRE(step.has_value());

  REQUIRE(step->from_entry_without_input().has_value());
  REQUIRE(step->set_static_value("request.kind", std::string{"direct"}).has_value());
}
