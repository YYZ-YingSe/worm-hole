#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph.hpp"

namespace {

using wh::testing::helper::collect_int_graph_chunk_values;
using wh::testing::helper::collect_int_graph_stream;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_auto_contract_edge_options;
using wh::testing::helper::make_int_add_node;
using wh::testing::helper::make_int_graph_stream;
using wh::testing::helper::read_any;
using wh::testing::helper::sum_ints;

} // namespace

TEST_CASE("compose graph mixed stream adapter matrix remains stable",
          "[core][compose][graph][stream][stress]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "left_source",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto typed = read_any<int>(input);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          typed.error());
                    }
                    return make_int_graph_stream({typed.value(), typed.value() + 1}, 2U);
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "right_source",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto typed = read_any<int>(input);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          typed.error());
                    }
                    return make_int_graph_stream({typed.value() + 10, typed.value() + 11}, 2U);
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "merged_value",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    int total = 0;
                    for (const auto &entry : merged.value()) {
                      auto chunks = collect_int_graph_chunk_values(entry.second);
                      if (chunks.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
                      }
                      total += sum_ints(chunks.value());
                    }
                    return wh::core::any(total);
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                  "count_sink",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(values.error());
                    }
                    return wh::core::any(static_cast<int>(values.value().size()));
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                  "sum_sink",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(values.error());
                    }
                    return wh::core::any(sum_ints(values.value()));
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "final_join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    auto count = read_any<int>(merged.value().at("count_sink"));
                    auto sum = read_any<int>(merged.value().at("sum_sink"));
                    if (count.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(count.error());
                    }
                    if (sum.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(sum.error());
                    }
                    return wh::core::any(count.value() + sum.value());
                  })
              .has_value());

  REQUIRE(graph.add_entry_edge("left_source").has_value());
  REQUIRE(graph.add_entry_edge("right_source").has_value());
  REQUIRE(
      graph.add_edge("left_source", "merged_value", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_edge("right_source", "merged_value", make_auto_contract_edge_options())
              .has_value());
  REQUIRE(
      graph.add_edge("merged_value", "count_sink", make_auto_contract_edge_options()).has_value());
  REQUIRE(
      graph.add_edge("merged_value", "sum_sink", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_edge("count_sink", "final_join").has_value());
  REQUIRE(graph.add_edge("sum_sink", "final_join").has_value());
  REQUIRE(graph.add_exit_edge("final_join").has_value());
  REQUIRE(graph.compile().has_value());

  for (int iteration = 0; iteration < 128; ++iteration) {
    wh::core::run_context context{};
    auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
    REQUIRE(invoked.has_value());
    auto typed = read_any<int>(invoked.value());
    REQUIRE(typed.has_value());
    REQUIRE(typed.value() == 4 * iteration + 23);
  }
}

TEST_CASE("compose graph one-to-one contract pair matrix remains stable",
          "[core][compose][graph][stream][stress]") {
  SECTION("value_to_value") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value>(
                    "stage",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                      }
                      return wh::core::any(typed.value() + 7);
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("stage").has_value());
    REQUIRE(graph.add_exit_edge("stage").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == iteration + 7);
    }
  }

  SECTION("value_to_stream") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "stage",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            typed.error());
                      }
                      return make_int_graph_stream({typed.value() + 1, typed.value() + 2}, 3U);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "sink",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(sum_ints(values.value()));
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("stage").has_value());
    REQUIRE(graph.add_edge("stage", "sink").has_value());
    REQUIRE(graph.add_exit_edge("sink").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 2 * iteration + 3);
    }
  }

  SECTION("stream_to_value") {
    wh::compose::graph graph{wh::compose::graph_boundary{
        .input = wh::compose::node_contract::stream, .output = wh::compose::node_contract::value}};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "stage",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(sum_ints(values.value()) + 5);
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("stage").has_value());
    REQUIRE(graph.add_exit_edge("stage").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(
          graph, wh::core::any(make_int_graph_stream({iteration, iteration + 1}, 3U).value()),
          context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 2 * iteration + 6);
    }
  }

  SECTION("stream_to_stream") {
    wh::compose::graph graph{wh::compose::graph_boundary{
        .input = wh::compose::node_contract::stream, .output = wh::compose::node_contract::value}};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::stream>(
                    "stage",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            values.error());
                      }
                      auto [writer, reader] = wh::compose::make_graph_stream(4U);
                      for (const auto value : values.value()) {
                        auto pushed = writer.try_write(wh::core::any(value * 2));
                        if (pushed.has_error()) {
                          return wh::core::result<wh::compose::graph_stream_reader>::failure(
                              pushed.error());
                        }
                      }
                      auto closed = writer.close();
                      if (closed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            closed.error());
                      }
                      return std::move(reader);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "sink",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(sum_ints(values.value()));
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("stage").has_value());
    REQUIRE(graph.add_edge("stage", "sink").has_value());
    REQUIRE(graph.add_exit_edge("sink").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(
          graph,
          wh::core::any(
              make_int_graph_stream({iteration, iteration + 1, iteration + 2}, 4U).value()),
          context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 6 * iteration + 6);
    }
  }
}

TEST_CASE("compose graph topology cardinality matrix remains stable",
          "[core][compose][graph][stream][stress]") {
  SECTION("one_to_many") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    wh::compose::graph graph{std::move(options)};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "source",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            typed.error());
                      }
                      return make_int_graph_stream(
                          {typed.value(), typed.value() + 1, typed.value() + 2}, 4U);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "sum_sink",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(sum_ints(values.value()));
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "count_sink",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(static_cast<int>(values.value().size()));
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "max_sink",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(
                          *std::max_element(values.value().begin(), values.value().end()));
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda(
                    "join",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto sum = read_any<int>(merged.value().at("sum_sink"));
                      auto count = read_any<int>(merged.value().at("count_sink"));
                      auto max = read_any<int>(merged.value().at("max_sink"));
                      if (sum.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(sum.error());
                      }
                      if (count.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(count.error());
                      }
                      if (max.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(max.error());
                      }
                      return wh::core::any(sum.value() + count.value() + max.value());
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("source").has_value());
    REQUIRE(graph.add_edge("source", "sum_sink").has_value());
    REQUIRE(graph.add_edge("source", "count_sink").has_value());
    REQUIRE(graph.add_edge("source", "max_sink").has_value());
    REQUIRE(graph.add_edge("sum_sink", "join").has_value());
    REQUIRE(graph.add_edge("count_sink", "join").has_value());
    REQUIRE(graph.add_edge("max_sink", "join").has_value());
    REQUIRE(graph.add_exit_edge("join").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 4 * iteration + 8);
    }
  }

  SECTION("stream_fan_in_value") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    wh::compose::graph graph{std::move(options)};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "left",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            typed.error());
                      }
                      return make_int_graph_stream({typed.value(), typed.value() + 1}, 3U);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "right",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            typed.error());
                      }
                      return make_int_graph_stream({typed.value() + 10, typed.value() + 11}, 3U);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda(
                    "join",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto left = collect_int_graph_chunk_values(merged.value().at("left"));
                      auto right = collect_int_graph_chunk_values(merged.value().at("right"));
                      if (left.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(left.error());
                      }
                      if (right.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(right.error());
                      }
                      return wh::core::any(sum_ints(left.value()) + sum_ints(right.value()) + 1);
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("left").has_value());
    REQUIRE(graph.add_entry_edge("right").has_value());
    REQUIRE(graph.add_edge("left", "join", make_auto_contract_edge_options()).has_value());
    REQUIRE(graph.add_edge("right", "join", make_auto_contract_edge_options()).has_value());
    REQUIRE(graph.add_exit_edge("join").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 4 * iteration + 23);
    }
  }

  SECTION("two_to_three") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    wh::compose::graph graph{std::move(options)};
    REQUIRE(graph.add_lambda(make_int_add_node("left", 1)).has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("right", 10)).has_value());
    REQUIRE(graph
                .add_lambda(
                    "join",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto left = read_any<int>(merged.value().at("left"));
                      auto right = read_any<int>(merged.value().at("right"));
                      if (left.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(left.error());
                      }
                      if (right.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(right.error());
                      }
                      return wh::core::any(left.value() + right.value());
                    })
                .has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("out_a", 1)).has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("out_b", 2)).has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("out_c", 3)).has_value());
    REQUIRE(graph
                .add_lambda(
                    "final",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      int total = 0;
                      for (const auto &name :
                           {std::string{"out_a"}, std::string{"out_b"}, std::string{"out_c"}}) {
                        auto typed = read_any<int>(merged.value().at(name));
                        if (typed.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                        }
                        total += typed.value();
                      }
                      return wh::core::any(total);
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("left").has_value());
    REQUIRE(graph.add_entry_edge("right").has_value());
    REQUIRE(graph.add_edge("left", "join").has_value());
    REQUIRE(graph.add_edge("right", "join").has_value());
    REQUIRE(graph.add_edge("join", "out_a").has_value());
    REQUIRE(graph.add_edge("join", "out_b").has_value());
    REQUIRE(graph.add_edge("join", "out_c").has_value());
    REQUIRE(graph.add_edge("out_a", "final").has_value());
    REQUIRE(graph.add_edge("out_b", "final").has_value());
    REQUIRE(graph.add_edge("out_c", "final").has_value());
    REQUIRE(graph.add_exit_edge("final").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 6 * iteration + 39);
    }
  }

  SECTION("three_to_two") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    wh::compose::graph graph{std::move(options)};
    REQUIRE(graph.add_lambda(make_int_add_node("a", 0)).has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("b", 1)).has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("c", 2)).has_value());
    REQUIRE(graph
                .add_lambda(
                    "join",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      int total = 0;
                      for (const auto &name :
                           {std::string{"a"}, std::string{"b"}, std::string{"c"}}) {
                        auto typed = read_any<int>(merged.value().at(name));
                        if (typed.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                        }
                        total += typed.value();
                      }
                      return wh::core::any(total);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda("left",
                            [](const wh::compose::graph_value &input, wh::core::run_context &,
                               const wh::compose::graph_call_scope &)
                                -> wh::core::result<wh::compose::graph_value> {
                              auto typed = read_any<int>(input);
                              if (typed.has_error()) {
                                return wh::core::result<wh::compose::graph_value>::failure(
                                    typed.error());
                              }
                              return wh::core::any(typed.value() * 2);
                            })
                .has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("right", 5)).has_value());
    REQUIRE(graph
                .add_lambda(
                    "final",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto left = read_any<int>(merged.value().at("left"));
                      auto right = read_any<int>(merged.value().at("right"));
                      if (left.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(left.error());
                      }
                      if (right.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(right.error());
                      }
                      return wh::core::any(left.value() + right.value());
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("a").has_value());
    REQUIRE(graph.add_entry_edge("b").has_value());
    REQUIRE(graph.add_entry_edge("c").has_value());
    REQUIRE(graph.add_edge("a", "join").has_value());
    REQUIRE(graph.add_edge("b", "join").has_value());
    REQUIRE(graph.add_edge("c", "join").has_value());
    REQUIRE(graph.add_edge("join", "left").has_value());
    REQUIRE(graph.add_edge("join", "right").has_value());
    REQUIRE(graph.add_edge("left", "final").has_value());
    REQUIRE(graph.add_edge("right", "final").has_value());
    REQUIRE(graph.add_exit_edge("final").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 9 * iteration + 14);
    }
  }

  SECTION("two_to_two") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    wh::compose::graph graph{std::move(options)};
    REQUIRE(graph.add_lambda(make_int_add_node("a", 1)).has_value());
    REQUIRE(graph.add_lambda(make_int_add_node("b", 2)).has_value());
    REQUIRE(graph
                .add_lambda(
                    "left",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto a = read_any<int>(merged.value().at("a"));
                      auto b = read_any<int>(merged.value().at("b"));
                      if (a.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(a.error());
                      }
                      if (b.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(b.error());
                      }
                      return wh::core::any(a.value() + b.value());
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda(
                    "right",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto a = read_any<int>(merged.value().at("a"));
                      auto b = read_any<int>(merged.value().at("b"));
                      if (a.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(a.error());
                      }
                      if (b.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(b.error());
                      }
                      return wh::core::any(a.value() * 10 + b.value());
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda(
                    "final",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto left = read_any<int>(merged.value().at("left"));
                      auto right = read_any<int>(merged.value().at("right"));
                      if (left.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(left.error());
                      }
                      if (right.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(right.error());
                      }
                      return wh::core::any(left.value() + right.value());
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("a").has_value());
    REQUIRE(graph.add_entry_edge("b").has_value());
    REQUIRE(graph.add_edge("a", "left").has_value());
    REQUIRE(graph.add_edge("b", "left").has_value());
    REQUIRE(graph.add_edge("a", "right").has_value());
    REQUIRE(graph.add_edge("b", "right").has_value());
    REQUIRE(graph.add_edge("left", "final").has_value());
    REQUIRE(graph.add_edge("right", "final").has_value());
    REQUIRE(graph.add_exit_edge("final").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 128; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 13 * iteration + 15);
    }
  }
}

TEST_CASE("compose graph stream extreme boundaries remain stable",
          "[core][compose][graph][stream][stress][boundary]") {
  SECTION("empty_stream_contract") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "source",
                    [](const wh::compose::graph_value &, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      return make_int_graph_stream({}, 1U);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "sink",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(static_cast<int>(values.value().size()));
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("source").has_value());
    REQUIRE(graph.add_edge("source", "sink").has_value());
    REQUIRE(graph.add_exit_edge("sink").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 32; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 0);
    }
  }

  SECTION("large_stream_fanout") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    wh::compose::graph graph{std::move(options)};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "source",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            typed.error());
                      }
                      auto [writer, reader] = wh::compose::make_graph_stream(257U);
                      for (int offset = 0; offset < 256; ++offset) {
                        auto pushed = writer.try_write(wh::core::any(typed.value() + offset));
                        if (pushed.has_error()) {
                          return wh::core::result<wh::compose::graph_stream_reader>::failure(
                              pushed.error());
                        }
                      }
                      auto closed = writer.close();
                      if (closed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            closed.error());
                      }
                      return std::move(reader);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "count_sink",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(static_cast<int>(values.value().size()));
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "sum_sink",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto values = collect_int_graph_stream(std::move(input));
                      if (values.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(values.error());
                      }
                      return wh::core::any(sum_ints(values.value()));
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda(
                    "join",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto count = read_any<int>(merged.value().at("count_sink"));
                      auto sum = read_any<int>(merged.value().at("sum_sink"));
                      if (count.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(count.error());
                      }
                      if (sum.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(sum.error());
                      }
                      return wh::core::any(count.value() + sum.value());
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("source").has_value());
    REQUIRE(graph.add_edge("source", "count_sink").has_value());
    REQUIRE(graph.add_edge("source", "sum_sink").has_value());
    REQUIRE(graph.add_edge("count_sink", "join").has_value());
    REQUIRE(graph.add_edge("sum_sink", "join").has_value());
    REQUIRE(graph.add_exit_edge("join").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 32; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 256 + 256 * iteration + 32640);
    }
  }

  SECTION("sparse_stream_fan_in_value") {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    wh::compose::graph graph{std::move(options)};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "left",
                    [](const wh::compose::graph_value &, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      return make_int_graph_stream({}, 1U);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "middle",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            typed.error());
                      }
                      auto [writer, reader] = wh::compose::make_graph_stream(33U);
                      for (int offset = 0; offset < 32; ++offset) {
                        auto pushed = writer.try_write(wh::core::any(typed.value() + offset));
                        if (pushed.has_error()) {
                          return wh::core::result<wh::compose::graph_stream_reader>::failure(
                              pushed.error());
                        }
                      }
                      auto closed = writer.close();
                      if (closed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            closed.error());
                      }
                      return std::move(reader);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "right",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      auto typed = read_any<int>(input);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            typed.error());
                      }
                      return make_int_graph_stream({typed.value() + 100}, 2U);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda(
                    "join",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto left = collect_int_graph_chunk_values(merged.value().at("left"));
                      auto middle = collect_int_graph_chunk_values(merged.value().at("middle"));
                      auto right = collect_int_graph_chunk_values(merged.value().at("right"));
                      if (left.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(left.error());
                      }
                      if (middle.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(middle.error());
                      }
                      if (right.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(right.error());
                      }
                      return wh::core::any(sum_ints(left.value()) + sum_ints(middle.value()) +
                                           sum_ints(right.value()));
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("left").has_value());
    REQUIRE(graph.add_entry_edge("middle").has_value());
    REQUIRE(graph.add_entry_edge("right").has_value());
    REQUIRE(graph.add_edge("left", "join", make_auto_contract_edge_options()).has_value());
    REQUIRE(graph.add_edge("middle", "join", make_auto_contract_edge_options()).has_value());
    REQUIRE(graph.add_edge("right", "join", make_auto_contract_edge_options()).has_value());
    REQUIRE(graph.add_exit_edge("join").has_value());
    REQUIRE(graph.compile().has_value());

    for (int iteration = 0; iteration < 64; ++iteration) {
      wh::core::run_context context{};
      auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
      REQUIRE(invoked.has_value());
      auto typed = read_any<int>(invoked.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 33 * iteration + 596);
    }
  }
}
