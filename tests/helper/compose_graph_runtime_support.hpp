// Test helpers for capturing exact runtime checkpoints through the public API.
#pragma once

#include <string>
#include <utility>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/start.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/passthrough.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::testing::helper {

[[nodiscard]] inline auto make_runtime_identity_graph(const wh::compose::graph_runtime_mode mode,
                                                      const std::string_view name = "runtime_graph")
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.name = std::string{name};
  options.mode = mode;
  wh::compose::graph graph{std::move(options)};
  auto added = graph.add_lambda(wh::compose::make_lambda_node(
      "worker",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
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

[[nodiscard]] inline auto
make_runtime_stream_identity_graph(const wh::compose::graph_runtime_mode mode,
                                   const std::string_view name = "runtime_stream_graph")
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.name = std::string{name};
  options.mode = mode;
  options.boundary = {
      .input = wh::compose::node_contract::stream,
      .output = wh::compose::node_contract::stream,
  };
  wh::compose::graph graph{std::move(options)};
  auto added = graph.add_passthrough<wh::compose::node_contract::stream>("worker");
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

[[nodiscard]] inline auto make_invoke_session(wh::compose::graph &graph,
                                              wh::compose::graph_value input,
                                              wh::core::run_context &context)
    -> wh::compose::detail::invoke_runtime::invoke_session {
  return wh::compose::detail::invoke_runtime::invoke_session{
      &graph,
      std::move(input),
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
}

[[nodiscard]] inline auto capture_exact_checkpoint(wh::compose::graph &graph,
                                                   wh::compose::graph_value input,
                                                   wh::core::run_context &context)
    -> wh::core::result<wh::compose::checkpoint_state> {
  const auto snapshot = graph.snapshot();
  if (snapshot.nodes.empty()) {
    return wh::core::result<wh::compose::checkpoint_state>::failure(wh::core::errc::not_found);
  }

  const auto target_node_key = snapshot.nodes.front().key;
  const auto checkpoint_id = std::string{graph.options().name} + "-capture";
  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);

  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.save = wh::compose::checkpoint_save_options{
      .checkpoint_id = checkpoint_id,
  };
  controls.interrupt.pre_hook = wh::compose::graph_interrupt_node_hook{
      [target_node_key, graph_name = graph.options().name](
          const std::string_view node_key, const wh::compose::graph_value &,
          wh::core::run_context &) -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
        if (node_key != target_node_key) {
          return std::optional<wh::core::interrupt_signal>{};
        }
        return std::optional<wh::core::interrupt_signal>{wh::compose::make_interrupt_signal(
            "capture-" + std::string{target_node_key},
            wh::core::make_address({graph_name, target_node_key}), std::monostate{})};
      }};

  auto invoked =
      invoke_graph_sync(graph, std::move(input), context, controls, std::addressof(services));
  if (invoked.has_error()) {
    return wh::core::result<wh::compose::checkpoint_state>::failure(invoked.error());
  }
  if (!invoked->output_status.has_error()) {
    return wh::core::result<wh::compose::checkpoint_state>::failure(wh::core::errc::internal_error);
  }
  if (invoked->output_status.error() != wh::core::errc::canceled) {
    return wh::core::result<wh::compose::checkpoint_state>::failure(invoked->output_status.error());
  }

  auto loaded = store.load(wh::compose::checkpoint_load_options{
      .checkpoint_id = checkpoint_id,
  });
  if (loaded.has_error()) {
    return wh::core::result<wh::compose::checkpoint_state>::failure(loaded.error());
  }
  loaded->resume_snapshot = wh::core::resume_state{};
  loaded->interrupt_snapshot = wh::core::interrupt_snapshot{};
  return std::move(loaded).value();
}

} // namespace wh::testing::helper
