#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/detail/inline_impl.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("inline impl umbrella exposes the fully linked graph inline runtime surface",
          "[UT][wh/compose/graph/detail/inline_impl.hpp][graph::compile][condition][branch][boundary]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker"))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());
  REQUIRE(graph.compiled());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(11);
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());
  auto invoke_status = std::get<0>(std::move(waited).value());
  REQUIRE(invoke_status.has_value());
  REQUIRE(invoke_status->output_status.has_value());
  auto *typed =
      wh::core::any_cast<int>(&invoke_status->output_status.value());
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 11);
}

TEST_CASE("inline impl umbrella keeps compile failure paths available through the same header surface",
          "[UT][wh/compose/graph/detail/inline_impl.hpp][graph::compile][branch][boundary]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough("worker").has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto second_compile = graph.compile();
  REQUIRE(second_compile.has_error());
  REQUIRE(second_compile.error() == wh::core::errc::contract_violation);
}
