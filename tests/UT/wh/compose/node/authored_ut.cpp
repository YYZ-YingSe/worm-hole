#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <variant>

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/authored.hpp"
#include "wh/compose/node/component.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/passthrough.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/compose/node/tools_builder.hpp"

namespace {

struct authored_component_stub {
  auto invoke(int value, wh::core::run_context &) const -> wh::core::result<int> {
    return value + 1;
  }
};

} // namespace

TEST_CASE("node_descriptor keeps stable default authored metadata",
          "[UT][wh/compose/node/authored.hpp][node_descriptor][boundary]") {
  const wh::compose::node_descriptor descriptor{};

  REQUIRE(descriptor.key.empty());
  REQUIRE(descriptor.kind == wh::compose::node_kind::component);
  REQUIRE(descriptor.exec_mode == wh::compose::node_exec_mode::sync);
  REQUIRE(descriptor.exec_origin ==
          wh::compose::default_exec_origin(wh::compose::node_kind::component));
  REQUIRE(descriptor.input_contract == wh::compose::node_contract::value);
  REQUIRE(descriptor.output_contract == wh::compose::node_contract::value);
  REQUIRE(descriptor.input_gate_info == wh::compose::input_gate::open());
  REQUIRE(descriptor.output_gate_info == wh::compose::output_gate::dynamic());
}

TEST_CASE("decorate_node_options fills only missing type and label fields",
          "[UT][wh/compose/node/authored.hpp][decorate_node_options][condition][branch][boundary]") {
  wh::compose::graph_add_node_options empty{};
  auto decorated =
      wh::compose::detail::decorate_node_options(empty, "component", "Component");
  REQUIRE(decorated.type == "component");
  REQUIRE(decorated.label == "Component");
  REQUIRE(decorated.name.empty());

  wh::compose::graph_add_node_options named{};
  auto decorated_named = wh::compose::detail::decorate_named_node_options(
      "worker", named, "lambda", "Lambda");
  REQUIRE(decorated_named.name == "worker");
  REQUIRE(decorated_named.type == "lambda");
  REQUIRE(decorated_named.label == "Lambda");

  wh::compose::graph_add_node_options explicit_options{};
  explicit_options.name = "kept";
  explicit_options.type = "explicit-type";
  explicit_options.label = "explicit-label";
  auto preserved = wh::compose::detail::decorate_named_node_options(
      "ignored", explicit_options, "fallback-type", "fallback-label");
  REQUIRE(preserved.name == "kept");
  REQUIRE(preserved.type == "explicit-type");
  REQUIRE(preserved.label == "explicit-label");
}

TEST_CASE("authored_node_like accepts the five authored node handles",
          "[UT][wh/compose/node/authored.hpp][authored_node_like][condition][boundary]") {
  STATIC_REQUIRE(wh::compose::authored_node_like<wh::compose::component_node>);
  STATIC_REQUIRE(wh::compose::authored_node_like<wh::compose::lambda_node>);
  STATIC_REQUIRE(wh::compose::authored_node_like<wh::compose::subgraph_node>);
  STATIC_REQUIRE(wh::compose::authored_node_like<wh::compose::tools_node>);
  STATIC_REQUIRE(wh::compose::authored_node_like<wh::compose::passthrough_node>);
  STATIC_REQUIRE_FALSE(wh::compose::authored_node_like<int>);
}

TEST_CASE("authored concrete nodes expose descriptors gates exec metadata and options",
          "[UT][wh/compose/node/authored.hpp][component_node::descriptor][condition][branch][boundary]") {
  auto component = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, int, int>("component",
                                                   authored_component_stub{});
  auto lambda = wh::compose::make_lambda_node(
      "lambda",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> { return input; });
  wh::compose::graph child{};
  auto subgraph = wh::compose::make_subgraph_node("subgraph", child);
  auto tools = wh::compose::make_tools_node("tools", wh::compose::tool_registry{});
  auto passthrough = wh::compose::make_passthrough_node("passthrough");

  REQUIRE(component.key() == "component");
  REQUIRE(component.input_contract() == wh::compose::node_contract::value);
  REQUIRE(component.output_contract() == wh::compose::node_contract::value);
  REQUIRE(component.input_gate().kind ==
          wh::compose::input_gate_kind::value_exact);
  REQUIRE(component.output_gate().kind ==
          wh::compose::output_gate_kind::value_exact);
  REQUIRE(component.exec_mode() == wh::compose::node_exec_mode::sync);
  REQUIRE(component.exec_origin() == wh::compose::node_exec_origin::authored);

  REQUIRE(lambda.key() == "lambda");
  REQUIRE(lambda.descriptor().kind == wh::compose::node_kind::lambda);
  REQUIRE(lambda.exec_origin() == wh::compose::node_exec_origin::authored);
  REQUIRE(subgraph.key() == "subgraph");
  REQUIRE(subgraph.descriptor().kind == wh::compose::node_kind::subgraph);
  REQUIRE(tools.key() == "tools");
  REQUIRE(tools.descriptor().kind == wh::compose::node_kind::tools);
  REQUIRE(passthrough.key() == "passthrough");
  REQUIRE(passthrough.descriptor().kind == wh::compose::node_kind::passthrough);
}

TEST_CASE("authored variant helpers visit key kind contracts gates and mutable options",
          "[UT][wh/compose/node/authored.hpp][authored_key][condition][branch][boundary]") {
  auto component = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, int, int>("component",
                                                   authored_component_stub{});
  auto passthrough = wh::compose::make_passthrough_node("passthrough");

  wh::compose::authored_node variant = passthrough;
  REQUIRE(wh::compose::authored_key(variant) == "passthrough");
  REQUIRE(wh::compose::authored_kind(variant) ==
          wh::compose::node_kind::passthrough);
  REQUIRE(wh::compose::authored_input_contract(variant) ==
          wh::compose::node_contract::value);
  REQUIRE(wh::compose::authored_output_contract(variant) ==
          wh::compose::node_contract::value);
  REQUIRE(wh::compose::detail::authored_input_gate(variant) ==
          wh::compose::input_gate::open());
  REQUIRE(wh::compose::detail::authored_output_gate(variant) ==
          wh::compose::output_gate::passthrough());

  auto &mutable_options = wh::compose::authored_options(variant);
  mutable_options.label = "passthrough-label";
  REQUIRE(wh::compose::authored_options(variant).label == "passthrough-label");
  REQUIRE(wh::compose::authored_options(component).name == "component");

  variant = component;
  REQUIRE(wh::compose::authored_key(variant) == "component");
  REQUIRE(wh::compose::authored_kind(variant) ==
          wh::compose::node_kind::component);
  REQUIRE(wh::compose::authored_input_contract(variant) ==
          wh::compose::node_contract::value);
  REQUIRE(wh::compose::authored_output_contract(variant) ==
          wh::compose::node_contract::value);
}
