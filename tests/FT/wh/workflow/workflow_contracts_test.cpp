#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/node/passthrough.hpp"
#include "wh/core/any.hpp"
#include "wh/workflow/workflow.hpp"

TEST_CASE("workflow facade authors compiles and invokes through public namespace",
          "[core][workflow][functional]") {
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

  auto *echo = wh::core::any_cast<int>(&std::get<0>(*awaited).value().at("input"));
  REQUIRE(echo != nullptr);
  REQUIRE(*echo == 42);
}

TEST_CASE("workflow facade supports public alias step refs and field mappings",
          "[core][workflow][functional]") {
  wh::workflow::workflow workflow{};
  auto source = workflow.add_step(wh::compose::make_passthrough_node("source"));
  REQUIRE(source.has_value());
  auto target = workflow.add_step(wh::compose::make_passthrough_node("target"));
  REQUIRE(target.has_value());

  const wh::workflow::workflow_step_ref source_ref = source.value();
  const wh::workflow::workflow_step_ref target_ref = target.value();
  REQUIRE(source_ref.from_entry().has_value());
  REQUIRE(target_ref
              .add_input(source_ref, {wh::workflow::field_mapping_rule{
                                         .from_path = "order.user.id",
                                         .to_path = "ctx.user_id",
                                     }})
              .has_value());
  REQUIRE(workflow.compile().has_value());

  wh::compose::graph_value_map input{};
  wh::compose::graph_value_map user{};
  user.insert_or_assign("id", wh::compose::graph_value{42});
  wh::compose::graph_value_map order{};
  order.insert_or_assign("user", wh::compose::graph_value{std::move(user)});
  input.insert_or_assign("order", wh::compose::graph_value{std::move(order)});

  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(workflow.invoke(context, std::move(input)));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());

  const auto &output = std::get<0>(*awaited).value();
  const auto ctx_iter = output.find("ctx");
  REQUIRE(ctx_iter != output.end());
  const auto *ctx_map = wh::core::any_cast<wh::compose::graph_value_map>(&ctx_iter->second);
  REQUIRE(ctx_map != nullptr);

  const auto user_id_iter = ctx_map->find("user_id");
  REQUIRE(user_id_iter != ctx_map->end());
  const auto *user_id = wh::core::any_cast<int>(&user_id_iter->second);
  REQUIRE(user_id != nullptr);
  REQUIRE(*user_id == 42);
}
