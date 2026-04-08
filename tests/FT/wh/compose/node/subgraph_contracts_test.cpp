#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/authored.hpp"
#include "wh/compose/node.hpp"

namespace {

using wh::testing::helper::build_single_node_graph;
using wh::testing::helper::execute_single_compiled_node;
using wh::testing::helper::make_test_node_runtime;
using wh::testing::helper::read_graph_value;

struct int_identity_component {
  auto invoke(int value, wh::core::run_context &) const
      -> wh::core::result<int> {
    return value;
  }
};

struct string_identity_component {
  auto invoke(std::string value, wh::core::run_context &) const
      -> wh::core::result<std::string> {
    return value;
  }
};

[[nodiscard]] auto make_streaming_subgraph_graph()
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph graph{
      wh::compose::graph_boundary{.output = wh::compose::node_contract::stream}};
  auto added = graph.add_lambda<wh::compose::node_contract::value,
                                wh::compose::node_contract::stream>(
      "emit",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_stream_reader> {
        auto typed = read_graph_value<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(
              typed.error());
        }
        auto [writer, reader] = wh::compose::make_graph_stream();
        auto wrote_first = writer.try_write(wh::core::any(typed.value()));
        if (wrote_first.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(
              wrote_first.error());
        }
        auto wrote_second = writer.try_write(wh::core::any(typed.value() + 1));
        if (wrote_second.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(
              wrote_second.error());
        }
        auto closed = writer.close();
        if (closed.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(
              closed.error());
        }
        return std::move(reader);
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  auto entry = graph.add_entry_edge("emit");
  if (entry.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(entry.error());
  }
  auto exit = graph.add_exit_edge("emit");
  if (exit.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(exit.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

} // namespace

TEST_CASE("compose subgraph node preserves explicit stream output boundary",
          "[core][compose][subgraph][boundary]") {
  auto child_graph = make_streaming_subgraph_graph();
  REQUIRE(child_graph.has_value());

  auto subgraph =
      wh::compose::make_subgraph_node("child", std::move(child_graph).value());
  auto lowered_subgraph = build_single_node_graph(subgraph);
  REQUIRE(lowered_subgraph.has_value());
  REQUIRE(lowered_subgraph->node->meta.input_contract ==
          wh::compose::node_contract::value);
  REQUIRE(lowered_subgraph->node->meta.output_contract ==
          wh::compose::node_contract::stream);
}

TEST_CASE("compose subgraph node executes real child graph through nested sender path",
          "[core][compose][subgraph][async]") {
  wh::compose::graph child{};
  REQUIRE(child
              .add_lambda(
                  "leaf",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &call_options)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto typed = read_graph_value<int>(input);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          typed.error());
                    }
                    auto value = typed.value() + 1;
                    if (call_options.trace().has_value() &&
                        call_options.trace()->trace_id == "nested") {
                      value += 10;
                    }
                    return wh::core::any(value);
                  })
              .has_value());
  REQUIRE(child.add_entry_edge("leaf").has_value());
  REQUIRE(child.add_exit_edge("leaf").has_value());
  REQUIRE(child.compile().has_value());

  auto subgraph = wh::compose::make_subgraph_node("child-graph", child);
  auto lowered = build_single_node_graph(subgraph);
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->node->meta.exec_mode == wh::compose::node_exec_mode::async);

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "nested",
      .parent_span_id = "nested-parent",
  };
  auto call_scope = wh::compose::graph_call_scope{call_options};

  wh::core::run_context context{};
  auto output = execute_single_compiled_node(
      subgraph, wh::core::any(5), context,
      make_test_node_runtime(std::addressof(call_scope)));
  REQUIRE(output.has_value());
  auto typed = read_graph_value<int>(std::move(output).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 16);
}

TEST_CASE("compose subgraph node executes child chain through shared nested invoke protocol",
          "[core][compose][subgraph][async]") {
  wh::compose::chain child{};
  REQUIRE(child
              .append(wh::compose::make_lambda_node(
                  "leaf",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &call_options)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto typed = read_graph_value<int>(input);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          typed.error());
                    }
                    auto value = typed.value() * 2;
                    if (call_options.trace().has_value() &&
                        call_options.trace()->trace_id == "chain-nested") {
                      value += 3;
                    }
                    return wh::core::any(value);
                  }))
              .has_value());
  REQUIRE(child.compile().has_value());

  auto subgraph = wh::compose::make_subgraph_node("child-chain", child);
  auto lowered = build_single_node_graph(subgraph);
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->node->meta.exec_mode == wh::compose::node_exec_mode::async);

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "chain-nested",
      .parent_span_id = "chain-parent",
  };
  auto call_scope = wh::compose::graph_call_scope{call_options};

  wh::core::run_context context{};
  auto output = execute_single_compiled_node(
      subgraph, wh::core::any(4), context,
      make_test_node_runtime(std::addressof(call_scope)));
  REQUIRE(output.has_value());
  auto typed = read_graph_value<int>(std::move(output).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 11);
}

TEST_CASE("compose subgraph graph_view wrapper bypasses public invoke path",
          "[core][compose][subgraph][async]") {
  wh::compose::graph child{};
  REQUIRE(child
              .add_lambda(
                  "leaf",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto typed = read_graph_value<int>(input);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          typed.error());
                    }
                    return wh::core::any(typed.value() + 2);
                  })
              .has_value());
  REQUIRE(child.add_entry_edge("leaf").has_value());
  REQUIRE(child.add_exit_edge("leaf").has_value());
  REQUIRE(child.compile().has_value());

  struct graph_view_wrapper {
    std::shared_ptr<wh::compose::graph> child{};
    std::shared_ptr<std::atomic<int>> public_invokes{};

    auto compile() -> wh::core::result<void> { return {}; }

    [[nodiscard]] auto graph_view() const noexcept -> const wh::compose::graph & {
      return *child;
    }

    [[nodiscard]] auto invoke(wh::core::run_context &,
                              const wh::compose::graph_value &) const
        -> wh::compose::graph_sender {
      public_invokes->fetch_add(1, std::memory_order_acq_rel);
      return wh::compose::graph_sender{stdexec::just(
          wh::core::result<wh::compose::graph_value>::failure(
              wh::core::errc::contract_violation))};
    }
  };

  auto public_invokes = std::make_shared<std::atomic<int>>(0);
  auto subgraph = wh::compose::make_subgraph_node(
      "wrapped-child",
      graph_view_wrapper{std::make_shared<wh::compose::graph>(child), public_invokes});

  wh::core::run_context context{};
  auto output =
      execute_single_compiled_node(subgraph, wh::core::any(5), context);
  INFO("public_invokes=" << public_invokes->load(std::memory_order_acquire));
  if (output.has_error()) {
    INFO("error=" << output.error());
  }
  REQUIRE(output.has_value());
  auto typed = read_graph_value<int>(std::move(output).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 7);
  REQUIRE(public_invokes->load(std::memory_order_acquire) == 0);
}

TEST_CASE("compose subgraph compile propagates child exact boundary gates",
          "[core][compose][subgraph][boundary]") {
  auto make_exact_int_child = []() -> wh::compose::graph {
    wh::compose::graph child{};
    REQUIRE(
        child.add_component(
                 wh::compose::make_component_node<
                     wh::core::component_kind::custom,
                     wh::compose::node_contract::value,
                     wh::compose::node_contract::value, int, int>(
                     "worker", int_identity_component{}))
            .has_value());
    REQUIRE(child.add_entry_edge("worker").has_value());
    REQUIRE(child.add_exit_edge("worker").has_value());
    return child;
  };

  SECTION("child exact input gate rejects mismatched parent source") {
    auto child = make_exact_int_child();

    wh::compose::graph parent{};
    REQUIRE(
        parent.add_component(
                  wh::compose::make_component_node<
                      wh::core::component_kind::custom,
                      wh::compose::node_contract::value,
                      wh::compose::node_contract::value, std::string,
                      std::string>("source", string_identity_component{}))
            .has_value());
    REQUIRE(parent
                .add_subgraph(
                    wh::compose::make_subgraph_node("child", std::move(child)))
                .has_value());
    REQUIRE(parent.add_entry_edge("source").has_value());
    REQUIRE(parent.add_edge("source", "child").has_value());
    REQUIRE(parent.add_exit_edge("child").has_value());

    auto compiled = parent.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
    REQUIRE(parent.diagnostics().back().message.find("source -> child") !=
            std::string::npos);
  }

  SECTION("child exact output gate rejects mismatched parent consumer") {
    auto child = make_exact_int_child();

    wh::compose::graph parent{};
    REQUIRE(parent
                .add_subgraph(
                    wh::compose::make_subgraph_node("child", std::move(child)))
                .has_value());
    REQUIRE(
        parent.add_component(
                  wh::compose::make_component_node<
                      wh::core::component_kind::custom,
                      wh::compose::node_contract::value,
                      wh::compose::node_contract::value, std::string,
                      std::string>("consumer", string_identity_component{}))
            .has_value());
    REQUIRE(parent.add_entry_edge("child").has_value());
    REQUIRE(parent.add_edge("child", "consumer").has_value());
    REQUIRE(parent.add_exit_edge("consumer").has_value());

    auto compiled = parent.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
    REQUIRE(parent.diagnostics().back().message.find("child -> consumer") !=
            std::string::npos);
  }
}
