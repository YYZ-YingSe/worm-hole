#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/runtime/run_state.hpp"

namespace {

class run_state_probe final : public wh::compose::detail::invoke_runtime::run_state {
public:
  using wh::compose::detail::invoke_runtime::run_state::run_state;
  using wh::compose::detail::invoke_runtime::run_state::init_error_;
  using wh::compose::detail::invoke_runtime::run_state::invoke_state;
  using wh::compose::detail::invoke_runtime::run_state::rebind_moved_runtime_storage;
};

} // namespace

TEST_CASE("runtime run_state start_graph_run launches compiled dag and pregel states",
          "[UT][wh/compose/graph/detail/runtime/run_state.hpp][start_graph_run][condition][branch][boundary]") {
  for (const auto mode : {wh::compose::graph_runtime_mode::dag,
                          wh::compose::graph_runtime_mode::pregel}) {
    auto graph = wh::testing::helper::make_runtime_identity_graph(mode);
    REQUIRE(graph.has_value());

    wh::core::run_context context{};
    auto state = wh::testing::helper::make_base_run_state(
        graph.value(), wh::compose::graph_value{5}, context);
    auto waited = stdexec::sync_wait(
        wh::compose::detail::invoke_runtime::start_graph_run(std::move(state)));
    REQUIRE(waited.has_value());

    auto status = std::get<0>(std::move(*waited));
    REQUIRE(status.has_value());
    auto typed =
        wh::testing::helper::read_graph_value<int>(std::move(status).value());
    REQUIRE(typed.has_value());
    REQUIRE(typed.value() == 5);
  }
}

TEST_CASE("runtime run_state preserves bound trace context after move-time storage rebinding",
          "[UT][wh/compose/graph/detail/runtime/run_state.hpp][run_state::rebind_moved_runtime_storage][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "run_state_rebind");
  REQUIRE(graph.has_value());

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "trace-id",
      .parent_span_id = "parent-span",
  };

  wh::core::run_context context{};
  run_state_probe state{
      &graph.value(),
      wh::compose::graph_value{3},
      context,
      std::move(call_options),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  REQUIRE(state.invoke_state().bound_call_scope.trace().has_value());
  REQUIRE(state.invoke_state().bound_call_scope.trace()->trace_id == "trace-id");

  run_state_probe moved{std::move(state)};
  moved.rebind_moved_runtime_storage();

  REQUIRE(moved.invoke_state().bound_call_scope.trace().has_value());
  REQUIRE(moved.invoke_state().bound_call_scope.trace()->trace_id == "trace-id");
  REQUIRE(moved.invoke_state().bound_call_scope.trace()->parent_span_id ==
          "parent-span");
}

TEST_CASE("runtime run_state captures initialization failures from invalid invoke controls",
          "[UT][wh/compose/graph/detail/runtime/run_state.hpp][run_state::run_state][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "run_state_invalid");
  REQUIRE(graph.has_value());

  wh::compose::graph_call_options call_options{};
  call_options.designated_top_level_nodes = {"missing"};

  wh::core::run_context context{};
  run_state_probe state{
      &graph.value(),
      wh::compose::graph_value{4},
      context,
      std::move(call_options),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };

  REQUIRE(state.init_error_.has_value());
  REQUIRE(*state.init_error_ == wh::core::errc::not_found);
}
