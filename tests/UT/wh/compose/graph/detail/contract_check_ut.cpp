#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/contract_check.hpp"
#include "wh/compose/node.hpp"

TEST_CASE("contract check helpers describe gates lift lowerings and validate compiled graphs",
          "[UT][wh/compose/graph/detail/"
          "contract_check.hpp][graph::validate_contracts][condition][branch][boundary]") {
  using namespace wh::compose::detail;

  const auto exact_int = wh::compose::value_gate::exact<int>();
  const auto exact_string = wh::compose::value_gate::exact<std::string>();
  REQUIRE(gate_text(wh::compose::input_gate::exact<int>()).find("value_exact") !=
          std::string::npos);
  REQUIRE(gate_text(wh::compose::output_gate::exact<int>()).find("value_exact") !=
          std::string::npos);
  REQUIRE(same_value_gate(exact_int, wh::compose::value_gate::exact<int>()));
  REQUIRE_FALSE(same_value_gate(exact_int, exact_string));

  wh::compose::detail::graph_core::indexed_edge passthrough_edge{};
  passthrough_edge.lowering_kind = wh::compose::edge_lowering_kind::none;
  auto passthrough_gate =
      lifted_value_gate(passthrough_edge, wh::compose::output_gate::exact<int>());
  REQUIRE(passthrough_gate.kind == wh::compose::output_gate_kind::value_exact);

  wh::compose::detail::graph_core::indexed_edge collect_edge{};
  collect_edge.lowering_kind = wh::compose::edge_lowering_kind::stream_to_value;
  auto collected_gate = lifted_value_gate(collect_edge, wh::compose::output_gate::reader());
  REQUIRE(collected_gate.kind == wh::compose::output_gate_kind::value_exact);
  REQUIRE(collected_gate.value.key() ==
          wh::core::any_type_key_v<std::vector<wh::compose::graph_value>>);

  REQUIRE(compatible_value_edge(passthrough_edge, wh::compose::output_gate::exact<int>(),
                                wh::compose::input_gate::exact<int>()));
  REQUIRE_FALSE(compatible_value_edge(passthrough_edge, wh::compose::output_gate::exact<int>(),
                                      wh::compose::input_gate::exact<std::string>()));
  REQUIRE(compatible_value_edge(
      collect_edge, wh::compose::output_gate::reader(),
      wh::compose::input_gate::exact<std::vector<wh::compose::graph_value>>()));

  auto incompatibility = incompatible_edge_message("left", "right", passthrough_edge,
                                                   wh::compose::output_gate::exact<int>(),
                                                   wh::compose::input_gate::exact<std::string>());
  REQUIRE(incompatibility.find("left -> right") != std::string::npos);

  wh::compose::detail::graph_core::graph_index index{};
  index.indexed_edges = {
      wh::compose::detail::graph_core::indexed_edge{
          .from = 0U,
          .to = 1U,
      },
      wh::compose::detail::graph_core::indexed_edge{
          .from = 0U,
          .to = 1U,
      },
  };
  index.incoming_data_edges.offsets = {0U, 0U, 2U};
  index.incoming_data_edges.edge_ids = {0U, 1U};

  std::vector<node_gate_state> gates(2U);
  gates[0U].resolved_output = wh::compose::output_gate::exact<int>();
  auto fan_in_gate = compiled_value_fan_in_gate(index, gates, 1U);
  REQUIRE(fan_in_gate.has_value());
  REQUIRE(fan_in_gate->kind == wh::compose::output_gate_kind::value_exact);
  REQUIRE(fan_in_gate->value.key() == wh::core::any_type_key_v<wh::compose::graph_value_map>);

  REQUIRE(accepts_fan_in_value_map(
      index, wh::compose::input_gate::exact<wh::compose::graph_value_map>(), 1U));
  REQUIRE_FALSE(accepts_fan_in_value_map(index, wh::compose::input_gate::exact<int>(), 1U));

  wh::compose::graph valid{};
  REQUIRE(valid.add_passthrough(wh::compose::make_passthrough_node("worker")).has_value());
  REQUIRE(valid.add_entry_edge("worker").has_value());
  REQUIRE(valid.add_exit_edge("worker").has_value());
  REQUIRE(valid.compile().has_value());
}

TEST_CASE(
    "contract check helpers cover dynamic lowerings reader compatibility and fan-in diagnostics",
    "[UT][wh/compose/graph/detail/"
    "contract_check.hpp][lifted_value_gate][condition][branch][boundary]") {
  using namespace wh::compose::detail;

  wh::compose::detail::graph_core::indexed_edge custom_edge{};
  custom_edge.lowering_kind = wh::compose::edge_lowering_kind::custom;
  auto custom_gate = lifted_value_gate(custom_edge, wh::compose::output_gate::reader());
  REQUIRE(custom_gate.kind == wh::compose::output_gate_kind::value_dynamic);

  wh::compose::detail::graph_core::indexed_edge value_to_stream_edge{};
  value_to_stream_edge.lowering_kind = wh::compose::edge_lowering_kind::value_to_stream;
  auto value_to_stream_gate =
      lifted_value_gate(value_to_stream_edge, wh::compose::output_gate::exact<int>());
  REQUIRE(value_to_stream_gate.kind == wh::compose::output_gate_kind::value_dynamic);

  REQUIRE(compatible_value_edge(custom_edge, wh::compose::output_gate::exact<int>(),
                                wh::compose::input_gate::reader()));
  REQUIRE(compatible_value_edge(custom_edge, wh::compose::output_gate::dynamic(),
                                wh::compose::input_gate::exact<std::string>()));

  wh::compose::detail::graph_core::graph_index empty_index{};
  std::vector<node_gate_state> empty_gates(1U);
  REQUIRE_FALSE(compiled_value_fan_in_gate(empty_index, empty_gates, 0U).has_value());

  auto fan_in_message =
      incompatible_value_fan_in_message("join", wh::compose::output_gate::exact<int>(),
                                        wh::compose::input_gate::exact<std::string>(), 3U);
  REQUIRE(fan_in_message.find("join") != std::string::npos);
  REQUIRE(fan_in_message.find("incoming=3") != std::string::npos);
}
