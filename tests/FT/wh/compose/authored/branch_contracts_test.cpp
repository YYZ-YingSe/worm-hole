#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/authored.hpp"

namespace {

using wh::testing::helper::invoke_value_sync;

template <typename value_t>
[[nodiscard]] auto any_get(const wh::core::any &value) noexcept
    -> const value_t * {
  return wh::core::any_cast<value_t>(&value);
}

} // namespace

TEST_CASE("compose branch supports stream-condition routing",
          "[core][compose][branch][condition]") {
  wh::compose::graph graph{
      wh::compose::graph_boundary{
          .input = wh::compose::node_contract::stream,
          .output = wh::compose::node_contract::stream,
      }};
  auto route =
      wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
          "route");
  auto stream_path =
      wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
          "stream-path");
  REQUIRE(graph.add_passthrough(std::move(route)).has_value());
  REQUIRE(graph.add_passthrough(std::move(stream_path)).has_value());
  REQUIRE(graph.add_entry_edge("route").has_value());
  REQUIRE(graph.add_exit_edge("stream-path").has_value());

  wh::compose::stream_branch branch{};
  REQUIRE(branch.add_target("stream-path").has_value());
  REQUIRE(branch
              .set_selector([](wh::compose::graph_stream_reader,
                               wh::core::run_context &)
                                -> wh::core::result<std::vector<std::string>> {
                return std::vector<std::string>{"stream-path"};
              })
              .has_value());
  REQUIRE(branch.apply(graph, "route").has_value());
  REQUIRE(graph.compile().has_value());

  auto stream_reader =
      wh::compose::make_single_value_stream_reader(wh::core::any(3));
  REQUIRE(stream_reader.has_value());
  wh::core::run_context context{};
  auto output = invoke_value_sync(graph, std::move(stream_reader).value(), context);
  REQUIRE(output.has_value());
  const auto *typed =
      any_get<wh::compose::graph_stream_reader>(output.value());
  REQUIRE(typed != nullptr);
}
