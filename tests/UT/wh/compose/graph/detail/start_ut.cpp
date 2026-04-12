#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/start.hpp"

namespace {

class run_state_probe final : public wh::compose::detail::invoke_runtime::run_state {
public:
  using wh::compose::detail::invoke_runtime::run_state::run_state;
  using wh::compose::detail::invoke_runtime::run_state::init_error_;
};

} // namespace

TEST_CASE("start graph run chooses dag or pregel sender by runtime mode and preserves output",
          "[UT][wh/compose/graph/detail/start.hpp][start_graph_run][condition][branch][boundary]") {
  auto dag_graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "start_dag");
  REQUIRE(dag_graph.has_value());
  wh::core::run_context dag_context{};
  auto dag_state = wh::testing::helper::make_base_run_state(
      dag_graph.value(), wh::compose::graph_value{15}, dag_context);
  auto dag_waited = stdexec::sync_wait(
      wh::compose::detail::invoke_runtime::start_graph_run(std::move(dag_state)));
  REQUIRE(dag_waited.has_value());
  REQUIRE(std::get<0>(*dag_waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*dag_waited).value()) == 15);

  auto pregel_graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "start_pregel");
  REQUIRE(pregel_graph.has_value());
  wh::core::run_context pregel_context{};
  auto pregel_state = wh::testing::helper::make_base_run_state(
      pregel_graph.value(), wh::compose::graph_value{16}, pregel_context);
  auto pregel_waited = stdexec::sync_wait(
      wh::compose::detail::invoke_runtime::start_graph_run(
          std::move(pregel_state)));
  REQUIRE(pregel_waited.has_value());
  REQUIRE(std::get<0>(*pregel_waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*pregel_waited).value()) == 16);
}

TEST_CASE("start graph run short-circuits immediate failures discovered during initialization",
          "[UT][wh/compose/graph/detail/start.hpp][start_graph_run][condition][branch]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "start_failure");
  REQUIRE(graph.has_value());

  wh::compose::graph_call_options call_options{};
  call_options.designated_top_level_nodes = {"missing"};

  wh::core::run_context context{};
  run_state_probe state{
      &graph.value(),
      wh::compose::graph_value{1},
      context,
      std::move(call_options),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  REQUIRE(state.init_error_.has_value());
  REQUIRE(*state.init_error_ == wh::core::errc::not_found);

  auto waited = stdexec::sync_wait(
      wh::compose::detail::invoke_runtime::start_graph_run(std::move(state)));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_error());
  REQUIRE(std::get<0>(*waited).error() == wh::core::errc::not_found);
}
