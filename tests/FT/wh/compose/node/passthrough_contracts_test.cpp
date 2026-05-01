#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/node/passthrough.hpp"

namespace {

using wh::testing::helper::build_single_node_graph;
using wh::testing::helper::execute_single_compiled_node;
using wh::testing::helper::read_graph_value;

} // namespace

TEST_CASE("compose passthrough node preserves explicit stream boundary",
          "[core][compose][passthrough][boundary]") {
  auto passthrough =
      wh::compose::make_passthrough_node<wh::compose::node_contract::stream>("passthrough");
  auto lowered_passthrough = build_single_node_graph(passthrough);
  REQUIRE(lowered_passthrough.has_value());
  REQUIRE(lowered_passthrough->node->meta.input_contract == wh::compose::node_contract::stream);
  REQUIRE(lowered_passthrough->node->meta.output_contract == wh::compose::node_contract::stream);

  auto [passthrough_writer, passthrough_reader] = wh::compose::make_graph_stream();
  REQUIRE(passthrough_writer.try_write(wh::core::any(8)).has_value());
  REQUIRE(passthrough_writer.close().has_value());

  wh::core::run_context context{};
  auto passthrough_output = execute_single_compiled_node(
      passthrough, wh::core::any(std::move(passthrough_reader)), context);
  REQUIRE(passthrough_output.has_value());
  auto passthrough_result =
      read_graph_value<wh::compose::graph_stream_reader>(std::move(passthrough_output).value());
  REQUIRE(passthrough_result.has_value());
  auto passthrough_chunks =
      wh::compose::collect_graph_stream_reader(std::move(passthrough_result).value());
  REQUIRE(passthrough_chunks.has_value());
  REQUIRE(passthrough_chunks.value().size() == 1U);
  auto passthrough_value = read_graph_value<int>(passthrough_chunks.value()[0]);
  REQUIRE(passthrough_value.has_value());
  REQUIRE(passthrough_value.value() == 8);
}
