#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/authored.hpp"
#include "wh/core/any.hpp"

namespace {

template <typename value_t>
[[nodiscard]] auto read_any(const wh::core::any &value)
    -> wh::core::result<value_t> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value);
      typed != nullptr) {
    if constexpr (std::copy_constructible<value_t>) {
      return *typed;
    } else {
      return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
    }
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] auto any_get(const wh::core::any &value) noexcept
    -> const value_t * {
  return wh::core::any_cast<value_t>(&value);
}

template <typename workflow_t, typename input_t>
[[nodiscard]] auto invoke_map_sync(const workflow_t &workflow, input_t &&input,
                                   wh::core::run_context &context)
    -> wh::core::result<wh::compose::graph_value_map> {
  auto sender = workflow.invoke(context, std::forward<input_t>(input));
  return wh::testing::helper::wait_sender_result<
      wh::core::result<wh::compose::graph_value_map>>(std::move(sender));
}

struct graph_copy_probe {
  static inline int copy_count = 0;
  static inline int move_count = 0;

  int payload{};

  graph_copy_probe() = default;
  explicit graph_copy_probe(const int value) : payload(value) {}

  graph_copy_probe(const graph_copy_probe &other) : payload(other.payload) {
    ++copy_count;
  }

  auto operator=(const graph_copy_probe &other) -> graph_copy_probe & {
    payload = other.payload;
    ++copy_count;
    return *this;
  }

  graph_copy_probe(graph_copy_probe &&other) noexcept : payload(other.payload) {
    ++move_count;
  }

  auto operator=(graph_copy_probe &&other) noexcept -> graph_copy_probe & {
    payload = other.payload;
    ++move_count;
    return *this;
  }
};

[[nodiscard]] auto make_uid_workflow() -> wh::core::result<wh::compose::workflow> {
  wh::compose::workflow workflow{};
  auto added_source = workflow.add_step(wh::compose::make_passthrough_node("source"));
  if (added_source.has_error()) {
    return wh::core::result<wh::compose::workflow>::failure(added_source.error());
  }
  auto added_target = workflow.add_step(wh::compose::make_passthrough_node("target"));
  if (added_target.has_error()) {
    return wh::core::result<wh::compose::workflow>::failure(added_target.error());
  }
  auto linked_start = added_source.value().from_entry();
  if (linked_start.has_error()) {
    return wh::core::result<wh::compose::workflow>::failure(linked_start.error());
  }
  auto linked_target = added_target.value().add_input(
      added_source.value(),
      {wh::compose::field_mapping_rule{
          .from_path = "order.user.id",
          .to_path = "ctx.uid",
          .missing_policy = wh::compose::field_missing_policy::fail,
      }});
  if (linked_target.has_error()) {
    return wh::core::result<wh::compose::workflow>::failure(linked_target.error());
  }
  auto compiled = workflow.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::workflow>::failure(compiled.error());
  }
  return workflow;
}

} // namespace

TEST_CASE("workflow compiles dependency mapping and applies nested path writes",
          "[core][compose][workflow][condition]") {
  wh::compose::workflow workflow{};
  auto source = workflow.add_step(wh::compose::make_passthrough_node("source"));
  REQUIRE(source.has_value());
  auto target = workflow.add_step(wh::compose::make_passthrough_node("target"));
  REQUIRE(target.has_value());
  REQUIRE(source.value().from_entry().has_value());
  REQUIRE(target.value()
              .add_input(source.value(),
                         {wh::compose::field_mapping_rule{
                             .from_path = "order.user.id",
                             .to_path = "ctx.uid",
                             .missing_policy =
                                 wh::compose::field_missing_policy::fail}})
              .has_value());
  REQUIRE(workflow.compile().has_value());

  wh::compose::graph_value_map input{};
  wh::compose::graph_value_map user_map{};
  user_map.insert_or_assign("id", wh::core::any(42));
  wh::compose::graph_value_map order_map{};
  order_map.insert_or_assign("user", wh::core::any(std::move(user_map)));
  input.insert_or_assign("order", wh::core::any(std::move(order_map)));

  wh::core::run_context context{};
  auto invoked = invoke_map_sync(workflow, input, context);
  REQUIRE(invoked.has_value());
  const auto ctx_iter = invoked.value().find("ctx");
  REQUIRE(ctx_iter != invoked.value().end());
  const auto *ctx_map =
      any_get<wh::compose::graph_value_map>(ctx_iter->second);
  REQUIRE(ctx_map != nullptr);
  const auto uid_iter = ctx_map->find("uid");
  REQUIRE(uid_iter != ctx_map->end());
  auto uid = read_any<int>(uid_iter->second);
  REQUIRE(uid.has_value());
  REQUIRE(uid.value() == 42);
}

TEST_CASE("workflow plain add_input reuses direct graph value-map fan-in",
          "[core][compose][workflow][condition]") {
  struct exact_int_component {
    auto invoke(wh::compose::graph_value_map, wh::core::run_context &) const
        -> wh::core::result<int> {
      return 7;
    }
  };

  wh::compose::workflow workflow{};
  auto route = workflow.add_step(wh::compose::make_lambda_node(
      "route",
      [](wh::compose::graph_value_map &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value_map> { return input; }));
  REQUIRE(route.has_value());
  auto count = workflow.add_step(wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, wh::compose::graph_value_map, int>(
      "count", exact_int_component{}));
  REQUIRE(count.has_value());
  auto join = workflow.add_step(wh::compose::make_lambda_node(
      "join",
      [](wh::compose::graph_value_map &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value_map> {
        return input;
      }));
  REQUIRE(join.has_value());

  REQUIRE(route.value().from_entry().has_value());
  REQUIRE(count.value().from_entry().has_value());
  REQUIRE(join.value().add_input(route.value()).has_value());
  REQUIRE(join.value().add_input(count.value()).has_value());
  REQUIRE(workflow.compile().has_value());

  const auto snapshot = workflow.graph_view().snapshot();
  REQUIRE(std::ranges::none_of(
      snapshot.nodes, [](const wh::compose::graph_snapshot_node &node) {
        return node.key == wh::compose::workflow_input_node_key("join");
      }));

  wh::compose::graph_value_map input{};
  input.insert_or_assign("payload", wh::core::any(9));
  wh::core::run_context context{};
  auto invoked = invoke_map_sync(workflow, std::move(input), context);
  REQUIRE(invoked.has_value());

  const auto route_iter = invoked.value().find("route");
  REQUIRE(route_iter != invoked.value().end());
  const auto *route_map =
      any_get<wh::compose::graph_value_map>(route_iter->second);
  REQUIRE(route_map != nullptr);
  const auto payload_iter = route_map->find("payload");
  REQUIRE(payload_iter != route_map->end());
  auto payload = read_any<int>(payload_iter->second);
  REQUIRE(payload.has_value());
  REQUIRE(payload.value() == 9);

  const auto count_iter = invoked.value().find("count");
  REQUIRE(count_iter != invoked.value().end());
  auto count_value = read_any<int>(count_iter->second);
  REQUIRE(count_value.has_value());
  REQUIRE(count_value.value() == 7);
}

TEST_CASE("workflow rejects unknown authored step lookup",
          "[core][compose][workflow][branch]") {
  wh::compose::workflow workflow{};
  REQUIRE(workflow.add_step(wh::compose::make_passthrough_node("a")).has_value());
  auto missing = workflow.step("ghost");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("workflow validates data-only dependencies and mapping path conflicts",
          "[core][compose][workflow][branch]") {
  wh::compose::workflow no_control_workflow{};
  auto a = no_control_workflow.add_step(wh::compose::make_passthrough_node("a"));
  REQUIRE(a.has_value());
  auto b = no_control_workflow.add_step(wh::compose::make_passthrough_node("b"));
  REQUIRE(b.has_value());
  REQUIRE(b.value()
              .add_input_without_control(
                  a.value(),
                  {wh::compose::field_mapping_rule{
                      .from_path = "order.id",
                      .to_path = "ctx.id",
                      .missing_policy = wh::compose::field_missing_policy::fail}})
              .has_value());
  auto no_control_compiled = no_control_workflow.compile();
  REQUIRE(no_control_compiled.has_error());
  REQUIRE(no_control_compiled.error() == wh::core::errc::contract_violation);

  wh::compose::workflow conflict_workflow{};
  auto conflict_a =
      conflict_workflow.add_step(wh::compose::make_passthrough_node("a"));
  REQUIRE(conflict_a.has_value());
  auto conflict_b =
      conflict_workflow.add_step(wh::compose::make_passthrough_node("b"));
  REQUIRE(conflict_b.has_value());
  REQUIRE(conflict_a.value().from_entry().has_value());
  auto conflict_added = conflict_b.value().add_input(
      conflict_a.value(),
      {wh::compose::field_mapping_rule{
           .from_path = "source.user",
           .to_path = "ctx",
           .missing_policy = wh::compose::field_missing_policy::fail},
       wh::compose::field_mapping_rule{
           .from_path = "source.user.id",
           .to_path = "ctx.user_id",
           .missing_policy = wh::compose::field_missing_policy::fail}});
  REQUIRE(conflict_added.has_error());
  REQUIRE(conflict_added.error() == wh::core::errc::already_exists);
}

TEST_CASE("workflow rejects multiple terminal steps without explicit join",
          "[core][compose][workflow][branch]") {
  wh::compose::workflow workflow{};
  auto left = workflow.add_step(wh::compose::make_passthrough_node("left"));
  REQUIRE(left.has_value());
  auto right = workflow.add_step(wh::compose::make_passthrough_node("right"));
  REQUIRE(right.has_value());
  REQUIRE(left.value().from_entry().has_value());
  REQUIRE(right.value().from_entry().has_value());

  auto compiled = workflow.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::contract_violation);
}

TEST_CASE("workflow authored value-branch convenience lowers through graph branch api",
          "[core][compose][workflow][branch]") {
  wh::compose::workflow workflow{};
  auto route_step = workflow.add_step(wh::compose::make_passthrough_node("route"));
  REQUIRE(route_step.has_value());
  auto left = workflow.add_step(wh::compose::make_passthrough_node("left"));
  REQUIRE(left.has_value());
  REQUIRE(route_step.value().from_entry().has_value());

  wh::compose::value_branch branch{};
  REQUIRE(branch
              .add_case(
                  "left",
                  [](const wh::compose::graph_value &input,
                     wh::core::run_context &)
                      -> wh::core::result<bool> {
                    auto mapped = wh::compose::payload_to_value_map_cref(input);
                    if (mapped.has_error()) {
                      return wh::core::result<bool>::failure(mapped.error());
                    }
                    const auto iter = mapped.value().get().find("route_left");
                    if (iter == mapped.value().get().end()) {
                      return wh::core::result<bool>::failure(
                          wh::core::errc::not_found);
                    }
                    const auto *route = any_get<bool>(iter->second);
                    if (route == nullptr) {
                      return wh::core::result<bool>::failure(
                          wh::core::errc::type_mismatch);
                    }
                    return *route;
                  })
              .has_value());
  REQUIRE(branch
              .add_case(
                  std::string{wh::compose::graph_end_node_key},
                  [](const wh::compose::graph_value &input,
                     wh::core::run_context &)
                      -> wh::core::result<bool> {
                    auto mapped = wh::compose::payload_to_value_map_cref(input);
                    if (mapped.has_error()) {
                      return wh::core::result<bool>::failure(mapped.error());
                    }
                    const auto iter = mapped.value().get().find("route_left");
                    if (iter == mapped.value().get().end()) {
                      return wh::core::result<bool>::failure(
                          wh::core::errc::not_found);
                    }
                    const auto *route = any_get<bool>(iter->second);
                    if (route == nullptr) {
                      return wh::core::result<bool>::failure(
                          wh::core::errc::type_mismatch);
                    }
                    return !*route;
                  })
              .has_value());
  REQUIRE(route_step.value().add_value_branch(std::move(branch)).has_value());
  REQUIRE(workflow.compile().has_value());

  wh::compose::graph_value_map input{};
  input.insert_or_assign("route_left", wh::core::any(true));
  input.insert_or_assign("payload", wh::core::any(7));
  wh::core::run_context context{};
  auto invoked = invoke_map_sync(workflow, std::move(input), context);
  REQUIRE(invoked.has_value());
  const auto route_iter = invoked.value().find("route_left");
  REQUIRE(route_iter != invoked.value().end());
  auto route_value = read_any<bool>(route_iter->second);
  REQUIRE(route_value.has_value());
  REQUIRE(route_value.value());
  const auto payload_iter = invoked.value().find("payload");
  REQUIRE(payload_iter != invoked.value().end());
  auto payload = read_any<int>(payload_iter->second);
  REQUIRE(payload.has_value());
  REQUIRE(payload.value() == 7);
}

TEST_CASE("workflow authored parallel convenience lowers through step api",
          "[core][compose][workflow][branch]") {
  wh::compose::workflow workflow{};
  auto source = workflow.add_step(wh::compose::make_passthrough_node("source"));
  REQUIRE(source.has_value());
  auto join = workflow.add_step(wh::compose::make_passthrough_node("join"));
  REQUIRE(join.has_value());
  REQUIRE(source.value().from_entry().has_value());

  wh::compose::parallel group{};
  REQUIRE(group.add_passthrough(wh::compose::make_passthrough_node("left")).has_value());
  REQUIRE(group.add_passthrough(wh::compose::make_passthrough_node("right"))
              .has_value());

  auto branches = source.value().add_parallel(std::move(group));
  REQUIRE(branches.has_value());
  REQUIRE(branches.value().size() == 2U);
  REQUIRE(join.value().add_input(branches.value()[0]).has_value());
  REQUIRE(join.value().add_input(branches.value()[1]).has_value());
  REQUIRE(workflow.compile().has_value());

  wh::compose::graph_value_map input{};
  input.insert_or_assign("payload", wh::core::any(9));
  wh::core::run_context context{};
  auto invoked = invoke_map_sync(workflow, std::move(input), context);
  REQUIRE(invoked.has_value());
  const auto left_iter = invoked.value().find("left");
  REQUIRE(left_iter != invoked.value().end());
  const auto *left_map = any_get<wh::compose::graph_value_map>(left_iter->second);
  REQUIRE(left_map != nullptr);
  const auto left_payload_iter = left_map->find("payload");
  REQUIRE(left_payload_iter != left_map->end());
  auto left_payload = read_any<int>(left_payload_iter->second);
  REQUIRE(left_payload.has_value());
  REQUIRE(left_payload.value() == 9);

  const auto right_iter = invoked.value().find("right");
  REQUIRE(right_iter != invoked.value().end());
  const auto *right_map =
      any_get<wh::compose::graph_value_map>(right_iter->second);
  REQUIRE(right_map != nullptr);
  const auto right_payload_iter = right_map->find("payload");
  REQUIRE(right_payload_iter != right_map->end());
  auto right_payload = read_any<int>(right_payload_iter->second);
  REQUIRE(right_payload.has_value());
  REQUIRE(right_payload.value() == 9);
}

TEST_CASE("workflow supports static value mapping dependency",
          "[core][compose][workflow][condition]") {
  wh::compose::workflow workflow{};
  auto added_source = workflow.add_step(wh::compose::make_passthrough_node("source"));
  REQUIRE(added_source.has_value());
  auto added_target = workflow.add_step(wh::compose::make_passthrough_node("target"));
  REQUIRE(added_target.has_value());
  REQUIRE(added_source.value().from_entry().has_value());
  REQUIRE(added_target.value()
              .add_input(
                  added_source.value(),
                  {wh::compose::field_mapping_rule{
                      .from_path = "foo",
                      .to_path = "ctx.foo",
                      .missing_policy = wh::compose::field_missing_policy::fail,
                  }})
              .has_value());
  REQUIRE(added_target.value()
              .set_static_value("ctx.static_id", wh::core::any(std::string{"ORD-001"}))
              .has_value());
  REQUIRE(workflow.compile().has_value());

  wh::compose::graph_value_map input{};
  input.insert_or_assign("foo", wh::core::any(1));
  wh::core::run_context context{};
  auto invoked = invoke_map_sync(workflow, input, context);
  REQUIRE(invoked.has_value());
  const auto ctx_iter = invoked.value().find("ctx");
  REQUIRE(ctx_iter != invoked.value().end());
  const auto *ctx =
      any_get<wh::compose::graph_value_map>(ctx_iter->second);
  REQUIRE(ctx != nullptr);
  const auto static_id_iter = ctx->find("static_id");
  REQUIRE(static_id_iter != ctx->end());
  auto static_id = read_any<std::string>(static_id_iter->second);
  REQUIRE(static_id.has_value());
  REQUIRE(static_id.value() == "ORD-001");
  const auto foo_iter = ctx->find("foo");
  REQUIRE(foo_iter != ctx->end());
  auto foo = read_any<int>(foo_iter->second);
  REQUIRE(foo.has_value());
  REQUIRE(foo.value() == 1);
}

TEST_CASE("workflow supports data-only mapped input with indirect control path",
          "[core][compose][workflow][condition]") {
  wh::compose::workflow workflow{};
  auto added_source = workflow.add_step(wh::compose::make_passthrough_node("source"));
  REQUIRE(added_source.has_value());
  auto added_gate = workflow.add_step(wh::compose::make_passthrough_node("gate"));
  REQUIRE(added_gate.has_value());
  auto added_target = workflow.add_step(wh::compose::make_passthrough_node("target"));
  REQUIRE(added_target.has_value());

  REQUIRE(added_source.value().from_entry().has_value());
  REQUIRE(added_gate.value().add_input(added_source.value()).has_value());
  REQUIRE(added_target.value().add_dependency(added_gate.value()).has_value());
  REQUIRE(added_target.value()
              .add_input_without_control(
                  added_source.value(),
                  {wh::compose::field_mapping_rule{
                      .from_path = "order.id",
                      .to_path = "ctx.id",
                      .missing_policy = wh::compose::field_missing_policy::fail,
                  }})
              .has_value());
  REQUIRE(workflow.compile().has_value());

  wh::compose::graph_value_map input{};
  wh::compose::graph_value_map order{};
  order.insert_or_assign("id", wh::core::any(42));
  input.insert_or_assign("order", wh::core::any(std::move(order)));

  wh::core::run_context context{};
  auto invoked = invoke_map_sync(workflow, input, context);
  REQUIRE(invoked.has_value());

  const auto ctx_iter = invoked.value().find("ctx");
  REQUIRE(ctx_iter != invoked.value().end());
  const auto *ctx = any_get<wh::compose::graph_value_map>(ctx_iter->second);
  REQUIRE(ctx != nullptr);
  const auto id_iter = ctx->find("id");
  REQUIRE(id_iter != ctx->end());
  auto id = read_any<int>(id_iter->second);
  REQUIRE(id.has_value());
  REQUIRE(id.value() == 42);
}

TEST_CASE("compiled workflow can be nested as subgraph",
          "[core][compose][workflow][condition]") {
  auto child = make_uid_workflow();
  REQUIRE(child.has_value());

  wh::compose::graph parent{};
  REQUIRE(parent
              .add_subgraph(
                  wh::compose::make_subgraph_node("child", std::move(child).value()))
              .has_value());
  REQUIRE(parent.add_entry_edge("child").has_value());
  REQUIRE(parent.add_exit_edge("child").has_value());
  REQUIRE(parent.compile().has_value());

  wh::compose::graph_value_map input{};
  wh::compose::graph_value_map user_map{};
  user_map.insert_or_assign("id", wh::core::any(42));
  wh::compose::graph_value_map order_map{};
  order_map.insert_or_assign("user", wh::core::any(std::move(user_map)));
  input.insert_or_assign("order", wh::core::any(std::move(order_map)));

  wh::core::run_context context{};
  auto invoked = wh::testing::helper::invoke_value_sync(
      parent, wh::compose::value_map_to_payload(std::move(input)), context);
  REQUIRE(invoked.has_value());
  auto output = wh::compose::payload_to_value_map(std::move(invoked).value());
  REQUIRE(output.has_value());
  const auto ctx_iter = output.value().find("ctx");
  REQUIRE(ctx_iter != output.value().end());
  const auto *ctx_map =
      any_get<wh::compose::graph_value_map>(ctx_iter->second);
  REQUIRE(ctx_map != nullptr);
  const auto uid_iter = ctx_map->find("uid");
  REQUIRE(uid_iter != ctx_map->end());
  auto uid = read_any<int>(uid_iter->second);
  REQUIRE(uid.has_value());
  REQUIRE(uid.value() == 42);
}

TEST_CASE("field mapping in place does not copy unrelated payloads",
          "[core][compose][workflow][condition]") {
  wh::compose::graph_value_map input{};
  input.insert_or_assign("heavy", wh::core::any(graph_copy_probe{7}));
  wh::compose::graph_value_map user_map{};
  user_map.insert_or_assign("id", wh::core::any(42));
  wh::compose::graph_value_map order_map{};
  order_map.insert_or_assign("user", wh::core::any(std::move(user_map)));
  input.insert_or_assign("order", wh::core::any(std::move(order_map)));

  auto compiled_rule = wh::compose::compile_field_mapping_rule(
      wh::compose::field_mapping_rule{
          .from_path = "order.user.id",
          .to_path = "ctx.uid",
          .missing_policy = wh::compose::field_missing_policy::fail,
      });
  REQUIRE(compiled_rule.has_value());
  std::vector<wh::compose::compiled_field_mapping_rule> rules{};
  rules.push_back(std::move(compiled_rule).value());

  graph_copy_probe::copy_count = 0;
  graph_copy_probe::move_count = 0;

  wh::core::run_context context{};
  auto updated =
      wh::compose::apply_field_mappings_in_place(input, rules, context);
  REQUIRE(updated.has_value());
  REQUIRE(graph_copy_probe::copy_count == 0);
  const auto heavy_iter = input.find("heavy");
  REQUIRE(heavy_iter != input.end());
  const auto *heavy = any_get<graph_copy_probe>(heavy_iter->second);
  REQUIRE(heavy != nullptr);
  REQUIRE(heavy->payload == 7);
  const auto ctx_iter = input.find("ctx");
  REQUIRE(ctx_iter != input.end());
  const auto *ctx =
      any_get<wh::compose::graph_value_map>(ctx_iter->second);
  REQUIRE(ctx != nullptr);
  const auto uid_iter = ctx->find("uid");
  REQUIRE(uid_iter != ctx->end());
  const auto *uid = any_get<int>(uid_iter->second);
  REQUIRE(uid != nullptr);
  REQUIRE(*uid == 42);
}
