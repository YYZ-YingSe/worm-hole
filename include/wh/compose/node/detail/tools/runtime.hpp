// Defines internal tools-node dispatch helpers.
#pragma once

#include <vector>

#include "wh/compose/node/detail/tools/call.hpp"
#include "wh/compose/node/detail/tools/output.hpp"
#include "wh/core/stdexec/concurrent_sender_vector.hpp"

namespace wh::compose {
namespace detail {

template <node_contract To>
[[nodiscard]] inline auto
run_tools_sync(const graph_value &input, wh::core::run_context &context,
               const node_runtime &runtime, const tool_registry &tools,
               const tools_options &options) -> wh::core::result<graph_value> {
  return with_node_context(
      context, runtime,
      [&](wh::core::run_context &node_context)
          -> wh::core::result<graph_value> {
        auto resolved =
            make_sync_tools_state(input, tools, options, node_context, runtime);
        if (resolved.has_error()) {
          return wh::core::result<graph_value>::failure(resolved.error());
        }
        auto state = std::move(resolved).value();

        if constexpr (To == node_contract::value) {
          std::vector<call_completion> results{};
          results.reserve(state.execute_indices.size());
          for (const auto index : state.execute_indices) {
            auto status = run_call(state, index, state.base_context());
            if (status.has_error()) {
              return wh::core::result<graph_value>::failure(status.error());
            }
            results.push_back(std::move(status).value());
          }
          return build_value_output(state, std::move(results));
        } else {
          std::vector<stream_completion> results{};
          results.reserve(state.execute_indices.size());
          for (const auto index : state.execute_indices) {
            auto status = run_stream_call(state, index, state.base_context());
            if (status.has_error()) {
              return wh::core::result<graph_value>::failure(status.error());
            }
            results.push_back(std::move(status).value());
          }
          return build_stream_output(state, std::move(results));
        }
      });
}

template <node_contract To>
[[nodiscard]] inline auto
run_tools_async(const graph_value &input, wh::core::run_context &context,
                const node_runtime &runtime, const tool_registry &tools,
                const tools_options &options) -> graph_sender {
  auto resolved =
      make_async_tools_state(input, tools, options, context, runtime);
  if (resolved.has_error()) {
    return ready_graph_sender(
        wh::core::result<graph_value>::failure(resolved.error()));
  }
  auto state = std::move(resolved).value();
  const auto max_in_flight =
      state.sequential
          ? 1U
          : resolve_parallel_call_budget(state.execute_indices.size(),
                                         runtime.parallel_gate());

  if constexpr (To == node_contract::value) {
    std::vector<call_completion_sender> senders{};
    senders.reserve(state.execute_indices.size());
    for (const auto index : state.execute_indices) {
      senders.push_back(start_call(state, index, state.base_context()));
    }
    auto sender =
        wh::core::detail::make_concurrent_sender_vector<
            wh::core::result<call_completion>>(std::move(senders),
                                               max_in_flight) |
        stdexec::then(
            [state = std::move(state)](
                std::vector<wh::core::result<call_completion>> results) mutable
                -> wh::core::result<graph_value> {
              std::vector<call_completion> values{};
              values.reserve(results.size());
              for (auto &result : results) {
                if (result.has_error()) {
                  return wh::core::result<graph_value>::failure(result.error());
                }
                values.push_back(std::move(result).value());
              }
              return build_value_output(state, std::move(values));
            });
    return bridge_graph_sender(std::move(sender));
  } else {
    std::vector<stream_completion_sender> senders{};
    senders.reserve(state.execute_indices.size());
    for (const auto index : state.execute_indices) {
      senders.push_back(start_stream_call(state, index, state.base_context()));
    }
    auto sender =
        wh::core::detail::make_concurrent_sender_vector<
            wh::core::result<stream_completion>>(std::move(senders),
                                                 max_in_flight) |
        stdexec::then(
            [state = std::move(state)](
                std::vector<wh::core::result<stream_completion>>
                    results) mutable -> wh::core::result<graph_value> {
              std::vector<stream_completion> values{};
              values.reserve(results.size());
              for (auto &result : results) {
                if (result.has_error()) {
                  return wh::core::result<graph_value>::failure(result.error());
                }
                values.push_back(std::move(result).value());
              }
              return build_stream_output(state, std::move(values));
            });
    return bridge_graph_sender(std::move(sender));
  }
}

} // namespace detail
} // namespace wh::compose
