// Defines tools-node output shaping helpers.
#pragma once

#include <utility>
#include <vector>

#include "wh/compose/node/detail/tools/state.hpp"
#include "wh/compose/node/detail/tools/tool_event_stream_reader.hpp"

namespace wh::compose {
namespace detail {

[[nodiscard]] inline auto build_value_output(tools_state &state,
                                             std::vector<call_completion> results)
    -> wh::core::result<graph_value> {
  std::vector<tool_result> output{};
  if (state.has_return_direct) {
    output.reserve(results.size());
  } else {
    output.resize(state.plans.size());
  }

  auto &rerun = state.rerun();
  for (auto &result : results) {
    rerun.outputs.insert_or_assign(result.call.call_id, result.value);
    rerun.extra.insert_or_assign(result.call.call_id, std::move(result.rerun_extra));
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

[[nodiscard]] inline auto build_stream_output(tools_state &state,
                                              std::vector<stream_completion> results)
    -> wh::core::result<graph_value> {
  auto &rerun = state.rerun();
  std::vector<tool_event_reader_detail::tool_event_binding> bindings{};
  std::vector<wh::schema::stream::named_stream_reader<graph_stream_reader>> lanes{};
  bindings.reserve(results.size());
  lanes.reserve(results.size());
  for (auto &result : results) {
    auto lane_source = result.call.call_id;
    rerun.extra.insert_or_assign(lane_source, std::move(result.rerun_extra));
    bindings.push_back(tool_event_reader_detail::tool_event_binding{
        .call = std::move(result.call),
        .context = std::move(result.after_context),
    });
    lanes.push_back({std::move(lane_source), std::move(result.stream), false});
  }

  auto merged = detail::make_graph_merge_reader(std::move(lanes));
  return wh::core::any(
      make_tools_output_stream_reader(std::move(merged), state.afters, std::move(bindings)));
}

} // namespace detail
} // namespace wh::compose
