#include <catch2/catch_test_macros.hpp>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/authored.hpp"

namespace {

using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_int_add_node;
using wh::testing::helper::read_any;

} // namespace

TEST_CASE("compose chain lowers parallel and branch syntax sugar to graph",
          "[core][compose][chain][branch]") {
  wh::compose::chain chain{};
  REQUIRE(chain.append(make_int_add_node("n1", 1)).has_value());

  wh::compose::parallel parallel{};
  auto n2 = make_int_add_node("n2", 10);
  n2.mutable_options().output_key = "n2";
  auto n3 = make_int_add_node("n3", 20);
  n3.mutable_options().output_key = "n3";
  REQUIRE(parallel.add_lambda(std::move(n2)).has_value());
  REQUIRE(parallel.add_lambda(std::move(n3)).has_value());
  REQUIRE(chain.append_parallel(parallel).has_value());

  REQUIRE(chain.compile().has_value());
  auto graph = chain.graph_view();
  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(1), context);
  REQUIRE(invoked.has_value());
  auto output = read_any<wh::compose::graph_value_map>(invoked.value());
  REQUIRE(output.has_value());
  REQUIRE(output.value().size() == 2U);
  auto n2_value = read_any<int>(output.value().at("n2"));
  auto n3_value = read_any<int>(output.value().at("n3"));
  REQUIRE(n2_value.has_value());
  REQUIRE(n3_value.has_value());
  REQUIRE(n2_value.value() == 12);
  REQUIRE(n3_value.value() == 22);
}

TEST_CASE("compose chain branch append requires unique predecessor tail",
          "[core][compose][chain][condition]") {
  wh::compose::chain chain{};
  REQUIRE(chain.append(make_int_add_node("n1", 1)).has_value());

  wh::compose::parallel parallel{};
  REQUIRE(parallel.add_lambda(make_int_add_node("n2", 1)).has_value());
  REQUIRE(parallel.add_lambda(make_int_add_node("n3", 1)).has_value());
  REQUIRE(chain.append_parallel(parallel).has_value());

  wh::compose::value_branch branch{};
  REQUIRE(branch
              .add_case("n2",
                        [](const wh::compose::graph_value &,
                           wh::core::run_context &) {
                          return true;
                        })
              .has_value());
  auto branch_result = chain.append_branch(branch);
  REQUIRE(branch_result.has_error());
  REQUIRE(branch_result.error() == wh::core::errc::contract_violation);
}
