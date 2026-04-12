#include <catch2/catch_test_macros.hpp>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/compose/runtime/state.hpp"

namespace {

auto nested_start(const void *, const wh::compose::graph &, wh::core::run_context &,
                  wh::compose::graph_value &, const wh::compose::graph_call_scope *,
                  const wh::compose::node_path *, wh::compose::graph_process_state *,
                  wh::compose::detail::runtime_state::invoke_outputs *,
                  const wh::compose::graph_node_trace *)
    -> wh::compose::graph_sender {
  return wh::compose::detail::ready_graph_unit_sender();
}

} // namespace

TEST_CASE("node_runtime_access bind_scope and bind_runtime expose public node_runtime views",
          "[UT][wh/compose/node/detail/runtime_access.hpp][node_runtime_access::bind_runtime][condition][branch][boundary]") {
  wh::compose::node_runtime runtime{};

  wh::compose::graph_call_options options{};
  wh::compose::graph_call_scope scope{options};
  auto path = wh::compose::make_node_path({"root", "leaf"});
  wh::compose::graph_process_state process_state{};
  wh::compose::graph_resolved_node_observation observation{};
  wh::compose::graph_node_trace trace{};
  trace.trace_id = "trace";
  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  const auto scheduler =
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});

  wh::compose::detail::node_runtime_access::bind_scope(runtime, &scope, &path);
  wh::compose::detail::node_runtime_access::bind_runtime(
      runtime, &scheduler, &process_state, &observation, &trace);

  REQUIRE(runtime.call_options() == &scope);
  REQUIRE(runtime.path() == &path);
  REQUIRE(runtime.graph_scheduler() == &scheduler);
  REQUIRE(runtime.process_state() == &process_state);
  REQUIRE(runtime.observation() == &observation);
  REQUIRE(runtime.trace() == &trace);
}

TEST_CASE("node_runtime_access bind_internal exposes invoke outputs and nested entry handles",
          "[UT][wh/compose/node/detail/runtime_access.hpp][node_runtime_access::bind_internal][condition][branch]") {
  wh::compose::node_runtime runtime{};
  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  wh::compose::nested_graph_entry entry{
      .state = &outputs,
      .start = &nested_start,
  };

  wh::compose::detail::node_runtime_access::bind_internal(runtime, &outputs,
                                                          entry);
  REQUIRE(
      wh::compose::detail::node_runtime_access::invoke_outputs(runtime) ==
      &outputs);
  REQUIRE(
      wh::compose::detail::node_runtime_access::nested_entry(runtime).bound());
}

TEST_CASE("node_runtime_access reset clears every bound pointer and replaces parallel gate",
          "[UT][wh/compose/node/detail/runtime_access.hpp][node_runtime_access::reset][condition][branch][boundary]") {
  wh::compose::node_runtime runtime{};
  runtime.set_parallel_gate(9U);
  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  wh::compose::nested_graph_entry entry{
      .state = &outputs,
      .start = &nested_start,
  };
  wh::compose::detail::node_runtime_access::bind_internal(runtime, &outputs,
                                                          entry);

  wh::compose::detail::node_runtime_access::reset(runtime, 3U);
  REQUIRE(runtime.parallel_gate() == 3U);
  REQUIRE(runtime.call_options() == nullptr);
  REQUIRE(runtime.path() == nullptr);
  REQUIRE(runtime.graph_scheduler() == nullptr);
  REQUIRE(runtime.process_state() == nullptr);
  REQUIRE(runtime.observation() == nullptr);
  REQUIRE(runtime.trace() == nullptr);
  REQUIRE(
      wh::compose::detail::node_runtime_access::invoke_outputs(runtime) ==
      nullptr);
  REQUIRE_FALSE(
      wh::compose::detail::node_runtime_access::nested_entry(runtime).bound());
}
