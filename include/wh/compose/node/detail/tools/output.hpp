// Defines tools-node output shaping helpers.
#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "wh/compose/node/detail/tools/call.hpp"
#include "wh/compose/node/detail/tools/stream.hpp"

namespace wh::compose {
namespace detail {

[[nodiscard]] inline auto
build_value_output(tools_state &state, std::vector<call_completion> results)
    -> wh::core::result<graph_value> {
  std::sort(results.begin(), results.end(),
            [](const call_completion &left, const call_completion &right) {
              return left.index < right.index;
            });

  std::vector<tool_result> output{};
  if (state.has_return_direct) {
    output.reserve(results.size());
  } else {
    output.resize(state.plans.size());
  }

  auto &rerun = state.rerun();
  for (auto &result : results) {
    rerun.outputs.insert_or_assign(result.call.call_id, result.value);
    rerun.extra.insert_or_assign(result.call.call_id,
                                 std::move(result.rerun_extra));
    tool_result entry{
        .call_id = result.call.call_id,
        .tool_name = result.call.tool_name,
        .value = std::move(result.value),
    };
    if (state.has_return_direct) {
      output.push_back(std::move(entry));
    } else {
      output[result.index] = std::move(entry);
    }
  }
  return wh::core::any(std::move(output));
}

[[nodiscard]] inline auto
build_stream_output(tools_state &state, std::vector<stream_completion> results)
    -> wh::core::result<graph_value> {
  std::sort(results.begin(), results.end(),
            [](const stream_completion &left, const stream_completion &right) {
              return left.index < right.index;
            });

  auto &rerun = state.rerun();
  auto afters = collect_tool_afters(*state.options);
  tool_stream_binding_map bindings{};
  bindings.reserve(results.size());
  std::vector<wh::schema::stream::named_stream_reader<graph_stream_reader>>
      lanes{};
  lanes.reserve(results.size());
  for (auto &result : results) {
    auto source = result.call.call_id;
    rerun.extra.insert_or_assign(result.call.call_id,
                                 std::move(result.rerun_extra));
    bindings.insert_or_assign(source, tool_stream_binding{
                                          .call = std::move(result.call),
                                          .context = std::move(result.context),
                                      });
    lanes.push_back({std::move(source), std::move(result.stream)});
  }

  auto merged = detail::make_graph_merge_reader(std::move(lanes));
  return wh::core::any(make_tool_event_stream_reader(
      std::move(merged), std::move(bindings), std::move(afters)));
}

} // namespace detail
} // namespace wh::compose
