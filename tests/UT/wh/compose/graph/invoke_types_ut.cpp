#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/invoke_types.hpp"

TEST_CASE("graph invoke types expose nested control and report structures",
          "[UT][wh/compose/graph/invoke_types.hpp][graph_invoke_request][condition][branch][boundary]") {
  wh::compose::graph_runtime_services services{};
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_value{7};
  request.controls.schedule.pregel_max_steps = 9U;
  request.services = &services;

  wh::compose::graph_invoke_result result{};
  result.output_status = wh::compose::graph_value{std::string{"ok"}};
  result.report.last_completed_nodes = {"a", "b"};

  REQUIRE(*wh::core::any_cast<int>(&request.input) == 7);
  REQUIRE(request.controls.schedule.pregel_max_steps == std::optional<std::size_t>{9U});
  REQUIRE(request.services == &services);
  REQUIRE(result.output_status.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&result.output_status.value()) == "ok");
  REQUIRE(result.report.last_completed_nodes.size() == 2U);
}

TEST_CASE("graph invoke type defaults keep optional service and control hooks empty",
          "[UT][wh/compose/graph/invoke_types.hpp][graph_invoke_controls][condition][branch][boundary]") {
  wh::compose::graph_runtime_services services{};
  wh::compose::graph_invoke_controls controls{};
  wh::compose::graph_run_report report{};

  REQUIRE(controls.checkpoint.load == std::nullopt);
  REQUIRE(controls.resume.decision == std::nullopt);
  REQUIRE(controls.resume.reinterrupt_unmatched);
  REQUIRE(controls.interrupt.pre_hook == nullptr);
  REQUIRE(services.checkpoint.store == nullptr);
  REQUIRE(report.transition_log.empty());
  REQUIRE(report.debug_events.empty());
}
