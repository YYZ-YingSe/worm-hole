#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/static_thread_scheduler.hpp"
#include "wh/compose/node/compiled.hpp"

namespace {

[[nodiscard]] auto await_graph_sender(wh::compose::graph_sender sender)
    -> wh::core::result<wh::compose::graph_value> {
  auto awaited = stdexec::sync_wait(std::move(sender));
  REQUIRE(awaited.has_value());
  return std::get<0>(std::move(*awaited));
}

} // namespace

TEST_CASE("compiled_node_meta keeps stable defaults for authored component value nodes",
          "[UT][wh/compose/node/compiled.hpp][compiled_node_meta][boundary]") {
  const wh::compose::compiled_node_meta meta{};
  REQUIRE(meta.key.empty());
  REQUIRE(meta.kind == wh::compose::node_kind::component);
  REQUIRE(meta.exec_mode == wh::compose::node_exec_mode::sync);
  REQUIRE(meta.exec_origin == wh::compose::default_exec_origin(wh::compose::node_kind::component));
  REQUIRE(meta.input_contract == wh::compose::node_contract::value);
  REQUIRE(meta.output_contract == wh::compose::node_contract::value);
  REQUIRE_FALSE(meta.compiled_input_gate.has_value());
  REQUIRE_FALSE(meta.compiled_output_gate.has_value());
  REQUIRE_FALSE(meta.subgraph_snapshot.has_value());
  REQUIRE_FALSE(meta.subgraph_restore_shape.has_value());
}

TEST_CASE(
    "compiled_sync_program returns not supported when no runner is bound",
    "[UT][wh/compose/node/compiled.hpp][compiled_sync_program::operator()][branch][boundary]") {
  wh::compose::graph_value input{3};
  wh::core::run_context context{};
  wh::compose::node_runtime runtime{};

  wh::compose::compiled_sync_program sync_program{};
  auto sync_status = sync_program(input, context, runtime);
  REQUIRE(sync_status.has_error());
  REQUIRE(sync_status.error() == wh::core::errc::not_supported);
}

TEST_CASE(
    "compiled_async_program returns not supported when no runner is bound",
    "[UT][wh/compose/node/compiled.hpp][compiled_async_program::operator()][branch][boundary]") {
  wh::compose::graph_value input{3};
  wh::core::run_context context{};
  wh::compose::node_runtime runtime{};

  wh::compose::compiled_async_program async_program{};
  auto async_status = await_graph_sender(async_program(input, context, runtime));
  REQUIRE(async_status.has_error());
  REQUIRE(async_status.error() == wh::core::errc::not_supported);
}

TEST_CASE(
    "compiled_node_is_sync and compiled_node_is_async reflect stored program variant",
    "[UT][wh/compose/node/compiled.hpp][compiled_node_is_sync][condition][branch][boundary]") {
  auto sync_node = wh::compose::make_compiled_sync_node(
      wh::compose::node_kind::lambda, wh::compose::node_exec_origin::authored,
      wh::compose::node_contract::value, wh::compose::node_contract::value, "sync",
      [](wh::compose::graph_value &value, wh::core::run_context &,
         const wh::compose::node_runtime &) -> wh::core::result<wh::compose::graph_value> {
        return value;
      });
  REQUIRE(wh::compose::compiled_node_is_sync(sync_node));
  REQUIRE_FALSE(wh::compose::compiled_node_is_async(sync_node));

  auto async_node = wh::compose::make_compiled_async_node(
      wh::compose::node_kind::lambda, wh::compose::node_exec_origin::authored,
      wh::compose::node_contract::value, wh::compose::node_contract::value, "async",
      [](wh::compose::graph_value &value, wh::core::run_context &,
         const wh::compose::node_runtime &) {
        return stdexec::just(wh::core::result<wh::compose::graph_value>{value});
      });
  REQUIRE(wh::compose::compiled_node_is_async(async_node));
  REQUIRE_FALSE(wh::compose::compiled_node_is_sync(async_node));
}

TEST_CASE(
    "make_compiled_sync_node preserves metadata options and executes bound runner",
    "[UT][wh/compose/node/compiled.hpp][make_compiled_sync_node][condition][branch][boundary]") {
  wh::compose::graph_add_node_options options{};
  options.name = "visible-name";
  options.label = "Visible Label";
  options.type = "compiled";

  auto sync_node = wh::compose::make_compiled_sync_node(
      wh::compose::node_kind::component, wh::compose::node_exec_origin::lowered,
      wh::compose::node_contract::value, wh::compose::node_contract::value, "worker",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::node_runtime &) -> wh::core::result<wh::compose::graph_value> {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return wh::compose::graph_value{*typed + 4};
      },
      options);

  REQUIRE(sync_node.meta.key == "worker");
  REQUIRE(sync_node.meta.kind == wh::compose::node_kind::component);
  REQUIRE(sync_node.meta.exec_mode == wh::compose::node_exec_mode::sync);
  REQUIRE(sync_node.meta.exec_origin == wh::compose::node_exec_origin::lowered);
  REQUIRE(sync_node.meta.options.name == "visible-name");
  REQUIRE(sync_node.meta.options.label == "Visible Label");
  REQUIRE(sync_node.meta.options.type == "compiled");

  wh::compose::graph_value sync_input{5};
  wh::core::run_context context{};
  auto sync_status = wh::compose::run_compiled_sync_node(sync_node, sync_input, context,
                                                         wh::compose::node_runtime{});
  REQUIRE(sync_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&sync_status.value()) == 9);

  auto wrong_sync = await_graph_sender(wh::compose::run_compiled_async_node(
      sync_node, sync_input, context, wh::compose::node_runtime{}));
  REQUIRE(wrong_sync.has_error());
  REQUIRE(wrong_sync.error() == wh::core::errc::contract_violation);
}

TEST_CASE(
    "make_compiled_async_node preserves metadata and dispatches async completion",
    "[UT][wh/compose/node/compiled.hpp][make_compiled_async_node][condition][branch][boundary]") {
  auto async_node = wh::compose::make_compiled_async_node(
      wh::compose::node_kind::lambda, wh::compose::node_exec_origin::authored,
      wh::compose::node_contract::value, wh::compose::node_contract::value, "async-worker",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::node_runtime &) {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return stdexec::just(
            wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{*typed + 7}});
      });

  wh::testing::helper::static_thread_scheduler_helper scheduler_helper{1U};
  auto scheduler = wh::core::detail::erase_resume_scheduler(scheduler_helper.scheduler());
  wh::compose::node_runtime runtime{};
  runtime.set_control_scheduler(&scheduler);

  wh::compose::graph_value async_input{2};
  wh::core::run_context context{};
  auto async_status = await_graph_sender(
      wh::compose::run_compiled_async_node(async_node, async_input, context, runtime));
  REQUIRE(async_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&async_status.value()) == 9);

  auto wrong_async = wh::compose::run_compiled_sync_node(async_node, async_input, context, runtime);
  REQUIRE(wrong_async.has_error());
  REQUIRE(wrong_async.error() == wh::core::errc::contract_violation);
}
