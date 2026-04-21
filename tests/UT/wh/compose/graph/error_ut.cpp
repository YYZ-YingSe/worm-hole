#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/error.hpp"

TEST_CASE("compose graph error helpers convert details into diagnostics",
          "[UT][wh/compose/graph/error.hpp][to_compose_error][condition][branch][boundary]") {
  const auto step_error = wh::compose::to_compose_error(wh::compose::graph_step_limit_error_detail{
      .step = 8U,
      .budget = 5U,
      .node = "node-a",
  });
  REQUIRE(step_error.code == wh::core::errc::timeout);
  REQUIRE(step_error.phase == wh::compose::compose_error_phase::schedule);
  REQUIRE(step_error.node == "node-a");

  const auto timeout_error =
      wh::compose::to_compose_error(wh::compose::graph_node_timeout_error_detail{
          .node = "node-b",
          .attempt = 2U,
          .timeout = std::chrono::milliseconds{50},
          .elapsed = std::chrono::milliseconds{80},
      });
  REQUIRE(timeout_error.phase == wh::compose::compose_error_phase::execute);

  auto diagnostic = wh::compose::to_graph_diagnostic(wh::compose::compose_error{
      .code = wh::core::make_error(wh::core::errc::canceled),
      .phase = wh::compose::compose_error_phase::resume,
      .node = "resume-node",
      .message = "stopped",
      .root_cause = wh::core::make_error(wh::core::errc::contract_violation),
  });
  REQUIRE(diagnostic.code == wh::core::errc::canceled);
  REQUIRE(diagnostic.message.find("phase=resume") != std::string::npos);
  REQUIRE(diagnostic.message.find("node=resume-node") != std::string::npos);
  REQUIRE(diagnostic.message.find("message=stopped") != std::string::npos);
}

TEST_CASE("compose graph diagnostic formatting omits absent node and root-cause fields",
          "[UT][wh/compose/graph/error.hpp][to_graph_diagnostic][condition][branch][boundary]") {
  auto diagnostic = wh::compose::to_graph_diagnostic(wh::compose::compose_error{
      .code = wh::core::make_error(wh::core::errc::timeout),
      .phase = wh::compose::compose_error_phase::compile,
      .message = "invalid",
  });

  REQUIRE(diagnostic.code == wh::core::errc::timeout);
  REQUIRE(diagnostic.message.find("phase=compile") != std::string::npos);
  REQUIRE(diagnostic.message.find("message=invalid") != std::string::npos);
  REQUIRE(diagnostic.message.find("node=") == std::string::npos);
  REQUIRE(diagnostic.message.find("root_cause=") == std::string::npos);
}
