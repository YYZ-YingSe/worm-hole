#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/runtime/invoke_session.hpp"

namespace {

class invoke_session_probe final : public wh::compose::detail::invoke_runtime::invoke_session {
public:
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_session;
  using wh::compose::detail::invoke_runtime::invoke_session::init_error_;
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_state;
  using wh::compose::detail::invoke_runtime::invoke_session::rebind_moved_runtime_storage;
};

} // namespace

TEST_CASE("runtime invoke_session launches compiled dag and pregel states through concrete entrypoints",
          "[UT][wh/compose/graph/detail/runtime/invoke_session.hpp][start_dag_run][start_pregel_run][condition][branch][boundary]") {
  for (const auto mode : {wh::compose::graph_runtime_mode::dag,
                          wh::compose::graph_runtime_mode::pregel}) {
    auto graph = wh::testing::helper::make_runtime_identity_graph(mode);
    REQUIRE(graph.has_value());

    wh::core::run_context context{};
    auto state = wh::testing::helper::make_invoke_session(
        graph.value(), wh::compose::graph_value{5}, context);
    auto waited = stdexec::sync_wait(mode == wh::compose::graph_runtime_mode::dag
                                         ? wh::compose::detail::invoke_runtime::
                                               start_dag_run(std::move(state))
                                         : wh::compose::detail::invoke_runtime::
                                               start_pregel_run(std::move(state)));
    REQUIRE(waited.has_value());

    auto status = std::get<0>(std::move(*waited));
    REQUIRE(status.has_value());
    auto typed =
        wh::testing::helper::read_graph_value<int>(std::move(status).value());
    REQUIRE(typed.has_value());
    REQUIRE(typed.value() == 5);
  }
}

TEST_CASE("runtime invoke_session preserves bound trace context after move-time storage rebinding",
          "[UT][wh/compose/graph/detail/runtime/invoke_session.hpp][invoke_session::rebind_moved_runtime_storage][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "invoke_session_rebind");
  REQUIRE(graph.has_value());

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "trace-id",
      .parent_span_id = "parent-span",
  };

  wh::core::run_context context{};
  invoke_session_probe state{
      &graph.value(),
      wh::compose::graph_value{3},
      context,
      std::move(call_options),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  REQUIRE(state.invoke_state().bound_call_scope.trace().has_value());
  REQUIRE(state.invoke_state().bound_call_scope.trace()->trace_id == "trace-id");

  invoke_session_probe moved{std::move(state)};
  moved.rebind_moved_runtime_storage();

  REQUIRE(moved.invoke_state().bound_call_scope.trace().has_value());
  REQUIRE(moved.invoke_state().bound_call_scope.trace()->trace_id == "trace-id");
  REQUIRE(moved.invoke_state().bound_call_scope.trace()->parent_span_id ==
          "parent-span");
}

TEST_CASE("runtime invoke_session captures initialization failures from invalid invoke controls",
          "[UT][wh/compose/graph/detail/runtime/invoke_session.hpp][invoke_session::invoke_session][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "invoke_session_invalid");
  REQUIRE(graph.has_value());

  wh::compose::graph_call_options call_options{};
  call_options.designated_top_level_nodes = {"missing"};

  wh::core::run_context context{};
  invoke_session_probe state{
      &graph.value(),
      wh::compose::graph_value{4},
      context,
      std::move(call_options),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };

  REQUIRE(state.init_error_.has_value());
  REQUIRE(*state.init_error_ == wh::core::errc::not_found);
}
