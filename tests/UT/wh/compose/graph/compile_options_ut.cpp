#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/compile_options.hpp"

TEST_CASE("graph compile options serialize all major policy fields",
          "[UT][wh/compose/graph/"
          "compile_options.hpp][serialize_graph_compile_options][condition][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.name = "graph-x";
  options.boundary = {.input = wh::compose::node_contract::stream,
                      .output = wh::compose::node_contract::value};
  options.mode = wh::compose::graph_runtime_mode::pregel;
  options.dispatch_policy = wh::compose::graph_dispatch_policy::next_wave;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  options.node_timeout = std::chrono::milliseconds{50};
  options.compile_callback = [](const wh::compose::graph_compile_info &) {
    return wh::core::result<void>{};
  };

  const auto text = wh::compose::serialize_graph_compile_options(options);
  REQUIRE(text.find("name=graph-x") != std::string::npos);
  REQUIRE(text.find("boundary=stream->value") != std::string::npos);
  REQUIRE(text.find("mode=pregel") != std::string::npos);
  REQUIRE(text.find("dispatch_policy=next_wave") != std::string::npos);
  REQUIRE(text.find("trigger_mode=all_predecessors") != std::string::npos);
  REQUIRE(text.find("fan_in_policy=require_all_sources") != std::string::npos);
  REQUIRE(text.find("compile_callback=true") != std::string::npos);
}

TEST_CASE("graph compile options defaults and false-path serialization remain stable",
          "[UT][wh/compose/graph/compile_options.hpp][graph_compile_options][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  REQUIRE(options.name == "graph");
  REQUIRE(options.mode == wh::compose::graph_runtime_mode::dag);
  REQUIRE(options.dispatch_policy == wh::compose::graph_dispatch_policy::same_wave);
  REQUIRE(options.trigger_mode == wh::compose::graph_trigger_mode::any_predecessor);
  REQUIRE(options.fan_in_policy == wh::compose::graph_fan_in_policy::allow_partial);
  REQUIRE(options.max_steps == 1024U);
  REQUIRE(options.retain_cold_data);
  REQUIRE(options.max_parallel_nodes == 1U);
  REQUIRE(options.max_parallel_per_node == 1U);
  REQUIRE(options.enable_local_state_generation);
  REQUIRE_FALSE(static_cast<bool>(options.compile_callback));

  options.retain_cold_data = false;
  options.enable_local_state_generation = false;
  const auto text = wh::compose::serialize_graph_compile_options(options);
  REQUIRE(text.find("mode=dag") != std::string::npos);
  REQUIRE(text.find("dispatch_policy=same_wave") != std::string::npos);
  REQUIRE(text.find("trigger_mode=any_predecessor") != std::string::npos);
  REQUIRE(text.find("fan_in_policy=allow_partial") != std::string::npos);
  REQUIRE(text.find("node_timeout_ms=none") != std::string::npos);
  REQUIRE(text.find("retain_cold_data=false") != std::string::npos);
  REQUIRE(text.find("enable_local_state_generation=false") != std::string::npos);
  REQUIRE(text.find("compile_callback=false") != std::string::npos);
}
