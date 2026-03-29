// Defines graph state-phase sync/async application over value and stream payloads.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline auto graph::apply_state_phase(
    wh::core::run_context &context, const graph_node_state_handlers *handlers,
    const detail::state_runtime::state_phase phase,
    const std::string_view node_key, const graph_state_cause &cause,
    graph_process_state &process_state, graph_value &payload,
    const node_path &runtime_path,
    detail::runtime_state::invoke_outputs &outputs) const
    -> wh::core::result<void> {
  return phase == detail::state_runtime::state_phase::pre
             ? detail::state_runtime::apply_pre_handlers(
                   context, handlers, node_key, cause, process_state, payload,
                   [this, &outputs, &runtime_path](wh::core::run_context &,
                                                   const std::string_view key,
                                                   const wh::core::error_code code,
                                                   const std::string_view message) {
                     publish_stream_read_error(outputs, runtime_path, key, code,
                                               message);
                   })
             : detail::state_runtime::apply_post_handlers(
                   context, handlers, node_key, cause, process_state, payload,
                   [this, &outputs, &runtime_path](wh::core::run_context &,
                                                   const std::string_view key,
                                                   const wh::core::error_code code,
                                                   const std::string_view message) {
                     publish_stream_read_error(outputs, runtime_path, key, code,
                                               message);
                   });
}

inline auto graph::apply_state_phase_async(
    wh::core::run_context &context, const graph_node_state_handlers *handlers,
    const detail::state_runtime::state_phase phase,
    const std::string_view node_key, const graph_state_cause &cause,
    graph_process_state &process_state, graph_value payload,
    const node_path &runtime_path,
    detail::runtime_state::invoke_outputs &outputs,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler) const
    -> graph_sender {
  using state_handler_t = detail::state_runtime::state_handler;

  struct phase_state {
    const graph *owner{nullptr};
    wh::core::run_context *context{nullptr};
    graph_process_state *process_state{nullptr};
    std::string node_key{};
    graph_state_cause cause{};
    node_path runtime_path{};
    detail::runtime_state::invoke_outputs *outputs{nullptr};
    detail::state_runtime::state_phase phase{
        detail::state_runtime::state_phase::pre};
    graph_value payload{};
    state_handler_t value_handler{nullptr};
    state_handler_t stream_handler{nullptr};
  };

  const auto fail_stream = [](phase_state &state,
                              const wh::core::error_code code,
                              const std::string_view message) -> graph_sender {
    state.owner->publish_stream_read_error(
        *state.outputs, state.runtime_path, state.node_key, code, message);
    return detail::failure_graph_sender(code);
  };

  const auto finish = [](phase_state state) -> graph_sender {
    return detail::ready_graph_sender(
        wh::core::result<graph_value>{std::move(state.payload)});
  };

  const auto run_stream =
      [finish, fail_stream, &graph_scheduler](phase_state state) -> graph_sender {
    if (state.stream_handler == nullptr) {
      return finish(std::move(state));
    }

    if (auto *reader = wh::core::any_cast<graph_stream_reader>(&state.payload);
        reader != nullptr) {
      auto source = std::move(*reader);
      auto [writer, rewritten] = make_graph_stream();
      auto handler = [cause = state.cause, context = state.context,
                      process_state = state.process_state,
                      stream_handler = state.stream_handler](
                         graph_value &chunk_payload) -> wh::core::result<void> {
        return stream_handler(cause, *process_state, chunk_payload, *context);
      };
      state.payload = wh::core::any(std::move(rewritten));
      return detail::bridge_graph_sender(
          wh::core::detail::bind_sender_scheduler(
              detail::make_rewrite_stream_sender(
                  std::move(source), std::move(writer), std::move(handler)) |
                  stdexec::let_value(
                      [state = std::move(state), finish,
                       fail_stream](wh::core::result<graph_value> status) mutable
                          -> graph_sender {
                        if (status.has_error()) {
                          return fail_stream(state, status.error(),
                                             "state_stream_read");
                        }
                        return finish(std::move(state));
                      }),
              wh::core::detail::any_resume_scheduler_t{graph_scheduler}));
    }

    auto handled = state.stream_handler(state.cause, *state.process_state,
                                        state.payload, *state.context);
    if (handled.has_error()) {
      return detail::failure_graph_sender(handled.error());
    }
    return finish(std::move(state));
  };

  phase_state state{
      .owner = this,
      .context = std::addressof(context),
      .process_state = std::addressof(process_state),
      .node_key = std::string{node_key},
      .cause = cause,
      .runtime_path = runtime_path,
      .outputs = std::addressof(outputs),
      .phase = phase,
      .payload = std::move(payload),
      .value_handler = detail::state_runtime::value_handler_for(handlers, phase),
      .stream_handler = detail::state_runtime::stream_handler_for(handlers, phase),
  };

  if (state.value_handler == nullptr) {
    return run_stream(std::move(state));
  }

  if (state.stream_handler == nullptr &&
      wh::core::any_cast<graph_stream_reader>(&state.payload) != nullptr) {
    auto reader = std::move(*wh::core::any_cast<graph_stream_reader>(&state.payload));
    return detail::bridge_graph_sender(
        collect_reader_value(std::move(reader), edge_limits{}, graph_scheduler) |
        stdexec::let_value(
            [state = std::move(state), run_stream,
             fail_stream](wh::core::result<graph_value> status) mutable
                -> graph_sender {
              if (status.has_error()) {
                return fail_stream(
                    state, status.error(),
                    state.phase == detail::state_runtime::state_phase::pre
                        ? "state_pre_aggregate_stream"
                        : "state_post_aggregate_stream");
              }
              state.payload = std::move(status).value();
              auto handled = state.value_handler(state.cause, *state.process_state,
                                                 state.payload, *state.context);
              if (handled.has_error()) {
                return detail::failure_graph_sender(handled.error());
              }
              return run_stream(std::move(state));
            }));
  }

  auto handled = state.value_handler(state.cause, *state.process_state,
                                     state.payload, *state.context);
  if (handled.has_error()) {
    return detail::failure_graph_sender(handled.error());
  }
  return run_stream(std::move(state));
}

} // namespace wh::compose
