#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "wh/compose/graph/error.hpp"
#include "wh/compose/graph/introspect.hpp"
#include "wh/compose/node.hpp"

TEST_CASE("compose introspect and error helpers preserve phase path and root-cause",
          "[core][compose][introspect][condition]") {
  wh::compose::compose_error error{};
  error.code = wh::core::errc::timeout;
  error.phase = wh::compose::compose_error_phase::checkpoint;
  error.node = "graph/worker";
  error.message = "checkpoint write failed";
  error.root_cause = wh::core::errc::network_error;
  auto diagnostic = wh::compose::to_graph_diagnostic(error);
  REQUIRE(diagnostic.code == wh::core::errc::timeout);
  REQUIRE(diagnostic.message.find("phase=checkpoint") != std::string::npos);
  REQUIRE(diagnostic.message.find("node=graph/worker") != std::string::npos);
  REQUIRE(diagnostic.message.find("root_cause=") != std::string::npos);

  wh::compose::graph_introspect_event event{};
  event.path = wh::compose::make_node_path({"root", "child"});
  event.phase = wh::compose::compose_error_phase::execute;
  event.code = wh::core::errc::contract_violation;
  event.root_cause = "downstream parse_error";
  event.message = "node execute failed";
  auto event_diagnostic = wh::compose::to_graph_diagnostic(event);
  REQUIRE(event_diagnostic.code == wh::core::errc::contract_violation);
  REQUIRE(event_diagnostic.message.find("phase=execute") != std::string::npos);
  REQUIRE(event_diagnostic.message.find("node=root/child") != std::string::npos);
  REQUIRE(event_diagnostic.message.find("root_cause=downstream parse_error") !=
          std::string::npos);

  auto step_error = wh::compose::to_compose_error(
      wh::compose::graph_step_limit_error_detail{
          .step = 10U,
          .budget = 8U,
          .node = "worker",
      });
  REQUIRE(step_error.code == wh::core::errc::timeout);
  REQUIRE(step_error.phase == wh::compose::compose_error_phase::schedule);
  REQUIRE(step_error.message.find("step-limit-exceeded") != std::string::npos);

  auto node_timeout_error = wh::compose::to_compose_error(
      wh::compose::graph_node_timeout_error_detail{
          .node = "worker",
          .attempt = 1U,
          .timeout = std::chrono::milliseconds{5},
          .elapsed = std::chrono::milliseconds{9},
      });
  REQUIRE(node_timeout_error.code == wh::core::errc::timeout);
  REQUIRE(node_timeout_error.phase == wh::compose::compose_error_phase::execute);
  REQUIRE(node_timeout_error.message.find("node-timeout") != std::string::npos);
}
