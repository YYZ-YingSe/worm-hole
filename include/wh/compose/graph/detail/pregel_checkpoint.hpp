// Defines Pregel runtime checkpoint capture helpers.
#pragma once

#include "wh/compose/graph/detail/runtime/checkpoint/pregel.hpp"
#include "wh/compose/graph/detail/runtime/pregel_runtime.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::pregel_runtime::capture_checkpoint_state()
    -> wh::core::result<checkpoint_state> {
  auto checkpoint = session_.capture_common_checkpoint_state();
  if (checkpoint.has_error()) {
    return wh::core::result<checkpoint_state>::failure(checkpoint.error());
  }
  auto captured = std::move(checkpoint).value();
  auto runtime_captured = capture_checkpoint_runtime(captured.runtime);
  if (runtime_captured.has_error()) {
    return wh::core::result<checkpoint_state>::failure(runtime_captured.error());
  }
  return captured;
}

inline auto detail::invoke_runtime::pregel_runtime::capture_checkpoint_runtime(
    checkpoint_runtime_state &runtime) -> wh::core::result<void> {
  auto &session = session_;
  auto pending_inputs = detail::checkpoint_runtime::capture_pending_inputs(
      session.pending_inputs_, session.compiled_graph_index().id_to_key,
      session.compiled_graph_index().nodes_by_id);
  if (pending_inputs.has_error()) {
    return wh::core::result<void>::failure(pending_inputs.error());
  }
  auto captured = detail::checkpoint_runtime::capture_pregel_runtime(
      session.io_storage_, pregel_delivery_, superstep_active_);
  if (captured.has_error()) {
    return wh::core::result<void>::failure(captured.error());
  }
  auto snapshot = std::move(captured).value();
  snapshot.pending_inputs = std::move(pending_inputs).value();
  runtime.pregel = std::move(snapshot);
  return {};
}

inline auto detail::invoke_runtime::pregel_runtime::try_persist_checkpoint() -> void {
  session_.request_persist_checkpoint();
}

inline auto detail::invoke_runtime::pregel_runtime::make_persist_sender() -> graph_sender {
  auto &invoke = session_.invoke_state();
  return detail::bridge_graph_sender(
      capture_pending_inputs() |
      stdexec::let_value([this, &invoke](wh::core::result<graph_value> captured) -> graph_sender {
        if (captured.has_error()) {
          return detail::failure_graph_sender(captured.error());
        }
        auto checkpoint = capture_checkpoint_state();
        if (checkpoint.has_error()) {
          detail::checkpoint_runtime::set_error_detail(invoke.outputs, checkpoint.error(),
                                                       session_.graph_options().name,
                                                       "capture_checkpoint_state");
          return detail::failure_graph_sender(checkpoint.error());
        }
        return session_.owner_->make_persist_checkpoint_sender(
            session_.context_, std::move(checkpoint).value(), invoke.config, invoke.outputs,
            *invoke.work_scheduler);
      }));
}

} // namespace wh::compose
