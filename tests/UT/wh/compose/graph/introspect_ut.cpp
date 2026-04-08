#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/introspect.hpp"

TEST_CASE("graph introspect event keeps stable default state",
          "[UT][wh/compose/graph/introspect.hpp][graph_introspect_event][boundary]") {
  const wh::compose::graph_introspect_event event{};

  REQUIRE(event.path.empty());
  REQUIRE(event.phase == wh::compose::compose_error_phase::execute);
  REQUIRE(event.code == wh::core::errc::ok);
  REQUIRE(event.root_cause.empty());
  REQUIRE(event.message.empty());
}

TEST_CASE("to_graph_diagnostic omits root cause suffix when event root cause is empty",
          "[UT][wh/compose/graph/introspect.hpp][to_graph_diagnostic][condition][branch][boundary]") {
  wh::compose::graph_introspect_event event{};
  event.path = wh::compose::make_node_path({"graph", "compile"});
  event.phase = wh::compose::compose_error_phase::compile;
  event.code = wh::core::errc::invalid_argument;
  event.message = "compile failed";

  const auto diagnostic = wh::compose::to_graph_diagnostic(event);
  REQUIRE(diagnostic.code == wh::core::errc::invalid_argument);
  REQUIRE(diagnostic.message.find("phase=compile") != std::string::npos);
  REQUIRE(diagnostic.message.find("node=graph/compile") != std::string::npos);
  REQUIRE(diagnostic.message.find("message=compile failed") !=
          std::string::npos);
  REQUIRE(diagnostic.message.find("bad-input") == std::string::npos);
}

TEST_CASE("to_graph_diagnostic appends root cause text into diagnostic message",
          "[UT][wh/compose/graph/introspect.hpp][to_graph_diagnostic][condition][branch]") {
  wh::compose::graph_introspect_event event{};
  event.path = wh::compose::make_node_path({"graph", "run"});
  event.phase = wh::compose::compose_error_phase::checkpoint;
  event.code = wh::core::errc::timeout;
  event.root_cause = "upstream-timeout";
  event.message = "checkpoint failed";

  const auto diagnostic = wh::compose::to_graph_diagnostic(event);
  REQUIRE(diagnostic.code == wh::core::errc::timeout);
  REQUIRE(diagnostic.message.find("phase=checkpoint") != std::string::npos);
  REQUIRE(diagnostic.message.find("node=graph/run") != std::string::npos);
  REQUIRE(diagnostic.message.find("checkpoint failed") != std::string::npos);
  REQUIRE(diagnostic.message.find("root_cause=upstream-timeout") !=
          std::string::npos);
}
