#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "wh/compose/node/passthrough.hpp"
#include "wh/workflow/workflow.hpp"

TEST_CASE("workflow facade reexports compose workflow aliases",
          "[UT][wh/workflow/workflow.hpp][workflow][boundary]") {
  STATIC_REQUIRE(std::same_as<wh::workflow::workflow, wh::compose::workflow>);
  STATIC_REQUIRE(std::same_as<wh::workflow::workflow_step_ref,
                              wh::compose::workflow::step_ref>);
  STATIC_REQUIRE(std::same_as<wh::workflow::field_mapping_rule,
                              wh::compose::field_mapping_rule>);
  STATIC_REQUIRE(std::same_as<wh::workflow::compiled_field_mapping_rule,
                              wh::compose::compiled_field_mapping_rule>);
  STATIC_REQUIRE(std::same_as<wh::workflow::workflow_dependency_kind,
                              wh::compose::workflow_dependency_kind>);
  STATIC_REQUIRE(std::same_as<wh::workflow::workflow_dependency,
                              wh::compose::workflow_dependency>);
}

TEST_CASE("workflow facade authors compiles and invokes the aliased compose workflow",
          "[UT][wh/workflow/workflow.hpp][workflow::compile][condition][branch][boundary]") {
  wh::workflow::workflow workflow{};
  auto step = workflow.add_step(wh::compose::make_passthrough_node("target"));
  REQUIRE(step.has_value());
  REQUIRE(step.value().from_entry().has_value());
  REQUIRE(workflow.end().add_input(step.value()).has_value());
  REQUIRE(workflow.compile().has_value());

  wh::compose::graph_value_map input{};
  input.insert_or_assign("input", wh::compose::graph_value{42});
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(workflow.invoke(context, input));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());

  auto *echo =
      wh::core::any_cast<int>(&std::get<0>(*awaited).value().at("input"));
  REQUIRE(echo != nullptr);
  REQUIRE(*echo == 42);
}
