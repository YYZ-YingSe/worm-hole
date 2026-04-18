#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "wh/compose/node/detail/tools/output.hpp"

namespace {

[[nodiscard]] auto make_graph_values(std::initializer_list<int> values)
    -> std::vector<wh::compose::graph_value> {
  std::vector<wh::compose::graph_value> output{};
  output.reserve(values.size());
  for (const auto value : values) {
    output.emplace_back(value);
  }
  return output;
}

} // namespace

TEST_CASE("build_value_output preserves indexed slots for non-return-direct plans",
          "[UT][wh/compose/node/detail/tools/output.hpp][build_value_output][condition][branch][boundary]") {
  wh::compose::detail::tools_state indexed_state{};
  wh::compose::tools_options indexed_options{};
  indexed_state.options = &indexed_options;
  indexed_state.plans.resize(3U);
  indexed_state.has_return_direct = false;

  auto indexed = wh::compose::detail::build_value_output(
      indexed_state,
      std::vector<wh::compose::detail::call_completion>{
          {.index = 0U,
           .call = {.call_id = "a", .tool_name = "alpha"},
           .value = wh::compose::graph_value{10},
           .rerun_extra = wh::compose::graph_value{std::string{"extra-a"}}},
          {.index = 2U,
           .call = {.call_id = "c", .tool_name = "gamma"},
           .value = wh::compose::graph_value{30},
           .rerun_extra = wh::compose::graph_value{std::string{"extra-c"}}},
      });
  REQUIRE(indexed.has_value());

  auto *indexed_results =
      wh::core::any_cast<std::vector<wh::compose::tool_result>>(
          &indexed.value());
  REQUIRE(indexed_results != nullptr);
  REQUIRE(indexed_results->size() == 3U);
  REQUIRE((*indexed_results)[0].call_id == "a");
  REQUIRE((*indexed_results)[0].tool_name == "alpha");
  REQUIRE(*wh::core::any_cast<int>(&(*indexed_results)[0].value) == 10);
  REQUIRE((*indexed_results)[1].call_id.empty());
  REQUIRE((*indexed_results)[2].call_id == "c");
  REQUIRE(*wh::core::any_cast<int>(&(*indexed_results)[2].value) == 30);

  REQUIRE(indexed_state.rerun().outputs.size() == 2U);
  REQUIRE(indexed_state.rerun().extra.size() == 2U);
  REQUIRE(*wh::core::any_cast<int>(&indexed_state.rerun().outputs.at("a")) == 10);
  REQUIRE(*wh::core::any_cast<std::string>(
              &indexed_state.rerun().extra.at("c")) == "extra-c");
}

TEST_CASE("build_value_output compacts direct-return outputs in sorted call order",
          "[UT][wh/compose/node/detail/tools/output.hpp][build_value_output][condition][branch]") {
  wh::compose::detail::tools_state direct_state{};
  wh::compose::tools_options direct_options{};
  direct_state.options = &direct_options;
  direct_state.has_return_direct = true;

  auto direct = wh::compose::detail::build_value_output(
      direct_state,
      std::vector<wh::compose::detail::call_completion>{
          {.index = 0U,
           .call = {.call_id = "a", .tool_name = "alpha"},
           .value = wh::compose::graph_value{10},
           .rerun_extra = wh::compose::graph_value{std::string{"extra-a"}}},
          {.index = 1U,
           .call = {.call_id = "b", .tool_name = "beta"},
           .value = wh::compose::graph_value{20},
           .rerun_extra = wh::compose::graph_value{std::string{"extra-b"}}},
      });
  REQUIRE(direct.has_value());
  auto *direct_results =
      wh::core::any_cast<std::vector<wh::compose::tool_result>>(&direct.value());
  REQUIRE(direct_results != nullptr);
  REQUIRE(direct_results->size() == 2U);
  REQUIRE((*direct_results)[0].call_id == "a");
  REQUIRE((*direct_results)[1].call_id == "b");
  REQUIRE(*wh::core::any_cast<std::string>(&direct_state.rerun().extra.at("a")) ==
          "extra-a");
}

TEST_CASE("build_value_output accepts empty completions and produces an empty result vector",
          "[UT][wh/compose/node/detail/tools/output.hpp][build_value_output][boundary]") {
  wh::compose::detail::tools_state state{};
  wh::compose::tools_options options{};
  state.options = &options;
  state.has_return_direct = true;

  auto output = wh::compose::detail::build_value_output(state, {});
  REQUIRE(output.has_value());
  auto *results =
      wh::core::any_cast<std::vector<wh::compose::tool_result>>(&output.value());
  REQUIRE(results != nullptr);
  REQUIRE(results->empty());
  REQUIRE(state.rerun().outputs.empty());
  REQUIRE(state.rerun().extra.empty());
}

TEST_CASE("build_stream_output merges per-call streams and applies after middleware",
          "[UT][wh/compose/node/detail/tools/output.hpp][build_stream_output][condition][branch][boundary]") {
  wh::compose::tools_options options{};
  options.middleware.push_back({
      .after =
          [](const wh::compose::tool_call &, wh::compose::graph_value &value,
             const wh::tool::call_scope &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<int>(&value);
        REQUIRE(typed != nullptr);
        value = wh::compose::graph_value{*typed + 100};
        return {};
      },
  });

  wh::compose::detail::tools_state state{};
  state.options = &options;
  state.afters = wh::compose::detail::make_tool_after_chain(options);

  std::vector<wh::compose::detail::stream_completion> completions{};
  completions.push_back({.index = 0U,
                         .call = {.call_id = "a", .tool_name = "alpha"},
                         .stream = wh::compose::make_values_stream_reader(
                             make_graph_values({1}))
                                       .value(),
                         .after_context = wh::core::run_context{},
                         .rerun_extra = wh::compose::graph_value{
                             std::string{"extra-a"}}});
  completions.push_back({.index = 1U,
                         .call = {.call_id = "b", .tool_name = "beta"},
                         .stream = wh::compose::make_values_stream_reader(
                             make_graph_values({2}))
                                       .value(),
                         .after_context = wh::core::run_context{},
                         .rerun_extra = wh::compose::graph_value{
                             std::string{"extra-b"}}});

  auto stream_output = wh::compose::detail::build_stream_output(
      state, std::move(completions));
  REQUIRE(stream_output.has_value());

  auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(
      &stream_output.value());
  REQUIRE(reader != nullptr);
  auto collected = wh::compose::collect_graph_stream_reader(std::move(*reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 2U);

  std::vector<wh::compose::tool_event> events{};
  for (auto &value : collected.value()) {
    auto *event = wh::core::any_cast<wh::compose::tool_event>(&value);
    REQUIRE(event != nullptr);
    events.push_back(*event);
  }
  std::sort(events.begin(), events.end(),
            [](const wh::compose::tool_event &left,
               const wh::compose::tool_event &right) {
              return left.call_id < right.call_id;
            });

  REQUIRE(events[0].call_id == "a");
  REQUIRE(events[0].tool_name == "alpha");
  REQUIRE(*wh::core::any_cast<int>(&events[0].value) == 101);
  REQUIRE(events[1].call_id == "b");
  REQUIRE(events[1].tool_name == "beta");
  REQUIRE(*wh::core::any_cast<int>(&events[1].value) == 102);

  REQUIRE(*wh::core::any_cast<std::string>(&state.rerun().extra.at("a")) ==
          "extra-a");
  REQUIRE(*wh::core::any_cast<std::string>(&state.rerun().extra.at("b")) ==
          "extra-b");
}

TEST_CASE("build_stream_output accepts empty completions and yields an empty merged reader",
          "[UT][wh/compose/node/detail/tools/output.hpp][build_stream_output][boundary]") {
  wh::compose::tools_options options{};
  wh::compose::detail::tools_state state{};
  state.options = &options;

  auto output = wh::compose::detail::build_stream_output(state, {});
  REQUIRE(output.has_value());
  auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(
      &output.value());
  REQUIRE(reader != nullptr);
  auto collected = wh::compose::collect_graph_stream_reader(std::move(*reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().empty());
  REQUIRE(state.rerun().extra.empty());
}
