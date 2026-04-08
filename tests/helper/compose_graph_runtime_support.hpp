#pragma once

#include <utility>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/detail/start.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::testing::helper {

[[nodiscard]] inline auto make_runtime_identity_graph(
    const wh::compose::graph_runtime_mode mode,
    const std::string_view name = "runtime_graph")
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.name = std::string{name};
  options.mode = mode;
  wh::compose::graph graph{std::move(options)};
  auto added = graph.add_lambda(wh::compose::make_lambda_node(
      "worker",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      }));
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  auto start = graph.add_entry_edge("worker");
  if (start.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(start.error());
  }
  auto finish = graph.add_exit_edge("worker");
  if (finish.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(finish.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] inline auto make_base_run_state(wh::compose::graph &graph,
                                              wh::compose::graph_value input,
                                              wh::core::run_context &context)
    -> wh::compose::detail::invoke_runtime::run_state {
  return wh::compose::detail::invoke_runtime::run_state{
      &graph,
      std::move(input),
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
}

} // namespace wh::testing::helper
