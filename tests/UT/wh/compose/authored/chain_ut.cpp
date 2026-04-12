#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/compose/authored/chain.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/passthrough.hpp"

namespace {

[[nodiscard]] auto make_subgraph() -> wh::compose::graph {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("inner"))
              .has_value());
  REQUIRE(graph.add_entry_edge("inner").has_value());
  REQUIRE(graph.add_exit_edge("inner").has_value());
  REQUIRE(graph.compile().has_value());
  return graph;
}

} // namespace

TEST_CASE("chain compiles invokes and releases lowered graph",
          "[UT][wh/compose/authored/chain.hpp][chain::invoke][condition][branch][boundary]") {
  wh::compose::chain chain{};
  const auto pass = wh::compose::make_passthrough_node("pass");
  REQUIRE(chain.append(pass).has_value());
  REQUIRE(chain.compile().has_value());
  REQUIRE(chain.graph_view().compiled());

  wh::compose::graph_value_map input{};
  input.insert_or_assign("value", wh::compose::graph_value{7});
  wh::core::run_context context{};
  auto awaited =
      stdexec::sync_wait(chain.invoke(context, wh::compose::graph_value{input}));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  auto *output =
      wh::core::any_cast<wh::compose::graph_value_map>(&std::get<0>(*awaited).value());
  REQUIRE(output != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&output->at("value")) == 7);

  auto released = std::move(chain).release_graph();
  REQUIRE(released.compiled());
}

TEST_CASE("chain validates empty compile contract and fail fast behavior",
          "[UT][wh/compose/authored/chain.hpp][chain::compile][condition][branch][boundary]") {
  wh::compose::chain empty{};
  auto empty_compile = empty.compile();
  REQUIRE(empty_compile.has_error());
  REQUIRE(empty_compile.error() == wh::core::errc::contract_violation);

  wh::compose::chain fail_fast{};
  auto invalid = fail_fast.append(wh::compose::make_passthrough_node(""));
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
  auto repeated = fail_fast.append(wh::compose::make_passthrough_node("later"));
  REQUIRE(repeated.has_error());
  REQUIRE(repeated.error() == wh::core::errc::invalid_argument);
}

TEST_CASE("chain routes branches parallel and subgraphs with tail validation",
          "[UT][wh/compose/authored/chain.hpp][chain::append_branch][condition][branch][boundary]") {
  wh::compose::value_branch branch{};
  REQUIRE(branch.add_case("left",
                          [](const wh::compose::graph_value &,
                             wh::core::run_context &) -> wh::core::result<bool> {
                            return true;
                          })
              .has_value());
  wh::compose::chain no_tail{};
  auto missing_tail = no_tail.append_branch(branch);
  REQUIRE(missing_tail.has_error());
  REQUIRE(missing_tail.error() == wh::core::errc::contract_violation);

  wh::compose::chain chain{};
  REQUIRE(chain.append(wh::compose::make_passthrough_node("source")).has_value());

  wh::compose::parallel group{};
  REQUIRE(group.add_passthrough(wh::compose::make_passthrough_node("left"))
              .has_value());
  REQUIRE(group.add_passthrough(wh::compose::make_passthrough_node("right"))
              .has_value());
  REQUIRE(chain.append_parallel(group).has_value());

  auto branch_on_multiple_tails = chain.append_branch(branch);
  REQUIRE(branch_on_multiple_tails.has_error());
  REQUIRE(branch_on_multiple_tails.error() == wh::core::errc::contract_violation);

  wh::compose::chain subgraph_chain{};
  REQUIRE(subgraph_chain.append_subgraph("sub", make_subgraph()).has_value());
}

TEST_CASE("chain rejects structural mutations after successful compile",
          "[UT][wh/compose/authored/chain.hpp][chain::append][condition][branch][boundary]") {
  wh::compose::chain chain{};
  REQUIRE(chain.append(wh::compose::make_passthrough_node("first")).has_value());
  REQUIRE(chain.compile().has_value());

  auto appended = chain.append(wh::compose::make_passthrough_node("late"));
  REQUIRE(appended.has_error());
  REQUIRE(appended.error() == wh::core::errc::contract_violation);
}
