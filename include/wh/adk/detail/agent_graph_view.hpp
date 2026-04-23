// Defines reusable ADK graph-view helpers that project native message-stream
// graphs onto canonical `agent_output` value graphs.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "wh/agent/agent.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message.hpp"

namespace wh::adk::detail {

using agent_output_from_messages =
    wh::core::callback_function<wh::core::result<wh::agent::agent_output>(
        std::vector<wh::schema::message>, wh::core::run_context &) const>;

[[nodiscard]] inline auto read_message_stream(wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<wh::schema::message>> {
  auto values = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (values.has_error()) {
    return wh::core::result<std::vector<wh::schema::message>>::failure(values.error());
  }

  std::vector<wh::schema::message> messages{};
  messages.reserve(values.value().size());
  for (auto &value : values.value()) {
    auto *typed = wh::core::any_cast<wh::schema::message>(&value);
    if (typed == nullptr) {
      return wh::core::result<std::vector<wh::schema::message>>::failure(
          wh::core::errc::type_mismatch);
    }
    messages.push_back(std::move(*typed));
  }
  return messages;
}

[[nodiscard]] inline auto make_message_stream_reader(std::vector<wh::schema::message> messages)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  std::vector<wh::compose::graph_value> values{};
  values.reserve(messages.size());
  for (auto &message : messages) {
    values.emplace_back(std::move(message));
  }
  return wh::compose::make_values_stream_reader(std::move(values));
}

[[nodiscard]] inline auto lower_agent_output_view(std::string graph_name,
                                                  wh::compose::graph native_graph,
                                                  agent_output_from_messages build_output)
    -> wh::core::result<wh::compose::graph> {
  if (native_graph.boundary().output != wh::compose::node_contract::stream) {
    return wh::core::result<wh::compose::graph>::failure(wh::core::errc::contract_violation);
  }

  auto compile_options = native_graph.compile_options_snapshot();
  compile_options.name = std::move(graph_name);
  if (compile_options.max_steps > 0U) {
    compile_options.max_steps += 2U;
  }

  wh::compose::graph lowered{wh::compose::graph_boundary{
                                 .input = native_graph.boundary().input,
                                 .output = wh::compose::node_contract::value,
                             },
                             std::move(compile_options)};

  auto native_node = wh::compose::make_subgraph_node("__native__", std::move(native_graph));
  auto native_added = lowered.add_subgraph(std::move(native_node));
  if (native_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(native_added.error());
  }

  auto project_output = wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                                      wh::compose::node_contract::value>(
      "__agent_output__",
      [build_output = std::move(build_output)](
          wh::compose::graph_stream_reader input, wh::core::run_context &context,
          const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto messages = read_message_stream(std::move(input));
        if (messages.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(messages.error());
        }
        auto output = build_output(std::move(messages).value(), context);
        if (output.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(output.error());
        }
        return wh::compose::graph_value{std::move(output).value()};
      });
  auto project_added = lowered.add_lambda(std::move(project_output));
  if (project_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(project_added.error());
  }

  auto start_added = lowered.add_entry_edge("__native__");
  if (start_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(start_added.error());
  }
  auto project_edge = lowered.add_edge("__native__", "__agent_output__");
  if (project_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(project_edge.error());
  }
  auto exit_added = lowered.add_exit_edge("__agent_output__");
  if (exit_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(exit_added.error());
  }

  return lowered;
}

} // namespace wh::adk::detail
