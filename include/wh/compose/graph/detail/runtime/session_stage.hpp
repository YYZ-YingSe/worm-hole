// Defines invoke-session node stage pipeline helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

inline auto
detail::invoke_runtime::invoke_session::make_input_frame(const std::uint32_t node_id,
                                                    const std::size_t step)
    -> wh::core::result<node_frame> {
  const auto &index = compiled_graph_index();
  const auto *node = index.nodes_by_id[node_id];
  if (node == nullptr) {
    return wh::core::result<node_frame>::failure(wh::core::errc::not_found);
  }

  node_frame frame{};
  frame.stage = invoke_stage::input;
  frame.node_id = node_id;
  frame.cause = graph_state_cause{
      .run_id = invoke_state().run_id,
      .step = step,
      .node_key = index.id_to_key[node_id],
  };
  frame.node = node;
  if (node_id < cache_state().resolved_state_handlers.size()) {
    frame.state_handlers = cache_state().resolved_state_handlers[node_id];
  }
  return frame;
}

inline auto detail::invoke_runtime::invoke_session::begin_state_pre(
    node_frame &&frame, graph_value input) -> wh::core::result<state_step> {
  auto &invoke = invoke_state();
  if (frame.node == nullptr) {
    return wh::core::result<state_step>::failure(wh::core::errc::not_found);
  }

  if (frame.pre_state_reader.has_value()) {
    input = wh::core::any(std::move(*frame.pre_state_reader));
    frame.pre_state_reader.reset();
  }

  const auto &current_node_key = node_key(frame.node_id);
  auto resume_interrupt = evaluate_resume_match(frame.node_id);
  if (resume_interrupt.has_error()) {
    return wh::core::result<state_step>::failure(resume_interrupt.error());
  }
  if (resume_interrupt.value().has_value()) {
    context_.interrupt_info = wh::compose::to_interrupt_context(
        std::move(resume_interrupt.value().value()));
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
               frame.node_id, frame.cause.step);
    pending_inputs_.store_input(frame.node_id, std::move(input));
    request_freeze(false);
    return wh::core::result<state_step>::failure(wh::core::errc::canceled);
  }

  auto pre_interrupt = owner_->evaluate_interrupt_hook(
      context_, invoke.config.interrupt_pre_hook, current_node_key, input);
  if (pre_interrupt.has_error()) {
    return wh::core::result<state_step>::failure(pre_interrupt.error());
  }
  if (pre_interrupt.value().has_value()) {
    context_.interrupt_info = wh::compose::to_interrupt_context(
        std::move(pre_interrupt.value().value()));
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
               frame.node_id, frame.cause.step);
    pending_inputs_.store_input(frame.node_id, std::move(input));
    request_freeze(false);
    return wh::core::result<state_step>::failure(wh::core::errc::canceled);
  }

  auto node_local_state =
      detail::process_runtime::acquire_node_local_process_state(
          node_local_process_states_, frame.node_id, process_state_);
  if (node_local_state.has_error()) {
    return wh::core::result<state_step>::failure(node_local_state.error());
  }
  auto node_local_scope = std::move(node_local_state).value();
  struct node_local_scope_guard {
    invoke_session *state{nullptr};
    detail::process_runtime::scoped_node_local_process_state *scope{nullptr};
    bool active{true};

    ~node_local_scope_guard() {
      if (active && state != nullptr && scope != nullptr) {
        scope->release(state->node_local_process_states_);
      }
    }

    auto dismiss() noexcept -> void { active = false; }
  };
  auto node_local_guard = node_local_scope_guard{
      .state = this,
      .scope = std::addressof(node_local_scope),
  };
  auto node_local_ref = node_local_scope.get(node_local_process_states_);
  if (node_local_ref.has_error()) {
    return wh::core::result<state_step>::failure(node_local_ref.error());
  }

  frame.node_scope.path = runtime_node_path(frame.node_id);
  frame.node_scope.local_process_state =
      std::addressof(node_local_ref.value().get());

  const auto skip_pre_state_handlers =
      restore_skip_pre_handlers_ && pending_inputs_.restored_node(frame.node_id);
  if (!skip_pre_state_handlers) {
    if (detail::state_runtime::needs_async_phase(
            frame.state_handlers, input,
            detail::state_runtime::state_phase::pre)) {
      auto sender = owner_->apply_state_phase_async(
          context_, frame.state_handlers,
          detail::state_runtime::state_phase::pre, current_node_key,
          frame.cause, node_local_ref.value().get(), std::move(input),
          frame.node_scope.path, invoke.outputs, *invoke.graph_scheduler);
      frame.stage = invoke_stage::pre_state;
      node_local_guard.dismiss();
      frame.node_local_scope = std::move(node_local_scope);
      return state_step{
          .frame = std::move(frame),
          .payload = {},
          .sender = std::move(sender),
      };
    }

    auto pre_state = owner_->apply_state_phase(
        context_, frame.state_handlers, detail::state_runtime::state_phase::pre,
        current_node_key, frame.cause, node_local_ref.value().get(), input,
        frame.node_scope.path, invoke.outputs);
    if (pre_state.has_error()) {
      owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                     frame.node_id, pre_state.error(),
                                     "node pre-state handler failed");
      state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                          pre_state.error());
      append_transition(frame.node_id,
                        graph_state_transition_event{
                            .kind = graph_state_transition_kind::node_fail,
                            .cause = frame.cause,
                            .lifecycle = graph_node_lifecycle_state::failed,
                        });
      return wh::core::result<state_step>::failure(pre_state.error());
    }
  }

  node_local_guard.dismiss();
  frame.node_local_scope = std::move(node_local_scope);
  return state_step{
      .frame = std::move(frame),
      .payload = std::move(input),
      .sender = std::nullopt,
  };
}

inline auto detail::invoke_runtime::invoke_session::prepare_execution_input(
    node_frame &&frame, graph_value input) -> wh::core::result<state_step> {
  if (!frame.input_lowering.has_value()) {
    return state_step{
        .frame = std::move(frame),
        .payload = std::move(input),
        .sender = std::nullopt,
    };
  }

  auto *reader = wh::core::any_cast<graph_stream_reader>(&input);
  if (reader == nullptr) {
    return wh::core::result<state_step>::failure(wh::core::errc::type_mismatch);
  }

  auto lowering = std::move(*frame.input_lowering);
  frame.input_lowering.reset();
  frame.stage = invoke_stage::prepare;
  return state_step{
      .frame = std::move(frame),
      .payload = {},
      .sender = owner_->lower_reader(std::move(*reader), std::move(lowering),
                                     context_, *invoke_state().graph_scheduler),
  };
}

inline auto detail::invoke_runtime::invoke_session::should_retain_input(
    const node_frame &frame) const noexcept -> bool {
  return invoke_state().retain_inputs || frame.retry_budget > 0U;
}

inline auto detail::invoke_runtime::invoke_session::finalize_node_frame(
    node_frame &&frame, graph_value input) -> wh::core::result<node_frame> {
  state_table_.update(frame.node_id, graph_node_lifecycle_state::running, 0U,
                      std::nullopt);
  append_transition(frame.node_id,
                    graph_state_transition_event{
                        .kind = graph_state_transition_kind::node_enter,
                        .cause = frame.cause,
                        .lifecycle = graph_node_lifecycle_state::running,
                    });

  const auto node_retry_budget =
      owner_->resolve_node_retry_budget(frame.node_id);
  const auto node_timeout_budget =
      owner_->resolve_node_timeout_budget(frame.node_id);
  const auto effective_parallel_gate = std::min(
      max_parallel_nodes(), owner_->resolve_node_parallel_gate(frame.node_id));
  if (effective_parallel_gate == 0U) {
    frame.node_local_scope.release(node_local_process_states_);
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        wh::core::errc::contract_violation);
    append_transition(frame.node_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause = frame.cause,
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    return wh::core::result<node_frame>::failure(
        wh::core::errc::contract_violation);
  }

  frame.stage = invoke_stage::node;
  frame.retry_budget = node_retry_budget;
  frame.timeout_budget = node_timeout_budget;
  if (should_retain_input(frame)) {
    auto retained_input =
        frame.node->meta.input_contract == node_contract::stream
            ? detail::fork_graph_reader_payload(input)
            : fork_graph_value(input);
    if (retained_input.has_error()) {
      const auto code =
          retained_input.error() == wh::core::errc::not_supported
              ? wh::core::errc::contract_violation
              : retained_input.error();
      frame.node_local_scope.release(node_local_process_states_);
      state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                          code);
      append_transition(frame.node_id,
                        graph_state_transition_event{
                            .kind = graph_state_transition_kind::node_fail,
                            .cause = frame.cause,
                            .lifecycle = graph_node_lifecycle_state::failed,
                        });
      return wh::core::result<node_frame>::failure(code);
    }
    pending_inputs_.store_input(frame.node_id, std::move(retained_input).value());
  }
  frame.node_input.emplace(std::move(input));
  detail::node_runtime_access::reset(frame.node_runtime,
                                     effective_parallel_gate);
  return frame;
}

inline auto detail::invoke_runtime::invoke_session::begin_state_post(
    node_frame &&frame, graph_value output) -> wh::core::result<state_step> {
  auto &invoke = invoke_state();
  if (detail::state_runtime::needs_async_phase(
          frame.state_handlers, output,
          detail::state_runtime::state_phase::post)) {
    auto sender = owner_->apply_state_phase_async(
        context_, frame.state_handlers,
        detail::state_runtime::state_phase::post, frame.cause.node_key,
        frame.cause, *frame.node_scope.local_process_state, std::move(output),
        frame.node_scope.path, invoke.outputs, *invoke.graph_scheduler);
    frame.stage = invoke_stage::post_state;
    return state_step{
        .frame = std::move(frame),
        .payload = {},
        .sender = std::move(sender),
    };
  }

  auto post_state = owner_->apply_state_phase(
      context_, frame.state_handlers, detail::state_runtime::state_phase::post,
      frame.cause.node_key, frame.cause, *frame.node_scope.local_process_state,
      output, frame.node_scope.path, invoke.outputs);
  if (post_state.has_error()) {
    owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                   frame.node_id, post_state.error(),
                                   "node post-state handler failed");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        post_state.error());
    append_transition(frame.node_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause = frame.cause,
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    frame.node_local_scope.release(node_local_process_states_);
    return wh::core::result<state_step>::failure(post_state.error());
  }
  return state_step{
      .frame = std::move(frame),
      .payload = std::move(output),
      .sender = std::nullopt,
  };
}

inline auto detail::invoke_runtime::invoke_session::fail_node_stage(
    node_frame &&frame, const wh::core::error_code code,
    const std::string_view message) -> wh::core::result<void> {
  owner_->publish_node_run_error(invoke_state().outputs, frame.node_scope.path,
                                 frame.node_id, code, message);
  state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                      code);
  append_transition(frame.node_id,
                    graph_state_transition_event{
                        .kind = graph_state_transition_kind::node_fail,
                        .cause = frame.cause,
                        .lifecycle = graph_node_lifecycle_state::failed,
                    });
  frame.node_local_scope.release(node_local_process_states_);
  return wh::core::result<void>::failure(code);
}

inline auto detail::invoke_runtime::invoke_session::bind_node_runtime_call_options(
    node_frame &frame, const graph_call_scope &bound_call_options,
    invoke_session *state) noexcept -> void {
  frame.node_scope.component_options =
      state->cache_state().resolved_component_options.empty()
          ? nullptr
          : std::addressof(
                state->cache_state().resolved_component_options[frame.node_id]);
  frame.node_scope.observation =
      state->cache_state().resolved_node_observations.empty()
          ? nullptr
          : std::addressof(
                state->cache_state().resolved_node_observations[frame.node_id]);
  frame.node_scope.trace = state->next_node_trace(frame.node_id);
  detail::node_runtime_access::bind_scope(
      frame.node_runtime, std::addressof(bound_call_options),
      std::addressof(frame.node_scope.path));
  detail::node_runtime_access::bind_runtime(
      frame.node_runtime,
      std::addressof(*state->invoke_state().graph_scheduler),
      frame.node_scope.local_process_state, frame.node_scope.observation,
      std::addressof(frame.node_scope.trace));
  detail::node_runtime_access::bind_internal(
      frame.node_runtime, std::addressof(state->invoke_state().outputs),
      state->nested_graph_entry());
}

inline auto detail::invoke_runtime::invoke_session::start_nested_from_runtime(
    const void *state_ptr, const graph &nested_graph,
    wh::core::run_context &context, graph_value &input,
    const graph_call_scope *call_options, const node_path *path_prefix,
    graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const graph_node_trace *parent_trace) -> graph_sender {
  const auto *state = static_cast<const invoke_session *>(state_ptr);
  auto nested_call_scope =
      call_options != nullptr
          ? graph_call_scope{call_options->options(), path_prefix != nullptr
                                                          ? *path_prefix
                                                          : node_path{}}
          : graph_call_scope{};
  if (parent_trace != nullptr) {
    auto trace = nested_call_scope.trace().value_or(graph_trace_context{});
    if (trace.trace_id.empty() && !parent_trace->trace_id.empty()) {
      trace.trace_id = std::string{parent_trace->trace_id};
    }
    trace.parent_span_id = parent_trace->span_id;
    nested_call_scope = nested_call_scope.with_trace(std::move(trace));
  }
  return detail::start_scoped_graph(
      nested_graph, context, input, std::addressof(nested_call_scope),
      path_prefix, parent_process_state, nested_outputs,
      *state->invoke_state().graph_scheduler, state);
}

inline auto
detail::invoke_runtime::invoke_session::nested_graph_entry() const noexcept
    -> wh::compose::nested_graph_entry {
  return wh::compose::nested_graph_entry{
      .state = this,
      .start = &start_nested_from_runtime,
  };
}

inline auto detail::invoke_runtime::invoke_session::timeout_scheduler() noexcept
    -> exec::timed_thread_scheduler {
  static exec::timed_thread_context context{};
  return context.get_scheduler();
}

inline auto detail::invoke_runtime::invoke_session::make_node_timeout_failure(
    detail::runtime_state::invoke_outputs &outputs,
    const std::string_view node_key, const std::size_t attempt,
    const std::chrono::milliseconds timeout_budget,
    const std::chrono::steady_clock::time_point attempt_start)
    -> wh::core::result<graph_value> {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - attempt_start);
  outputs.node_timeout_error = graph_node_timeout_error_detail{
      .node = std::string{node_key},
      .attempt = attempt,
      .timeout = timeout_budget,
      .elapsed = elapsed,
  };
  return wh::core::result<graph_value>::failure(wh::core::errc::timeout);
}

inline auto detail::invoke_runtime::invoke_session::apply_node_timeout(
    detail::runtime_state::invoke_outputs &outputs, const node_frame &frame,
    const std::chrono::steady_clock::time_point attempt_start,
    wh::core::result<graph_value> executed) -> wh::core::result<graph_value> {
  if (!frame.timeout_budget.has_value()) {
    return executed;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - attempt_start);
  if (elapsed <= *frame.timeout_budget) {
    return executed;
  }
  return make_node_timeout_failure(outputs, frame.cause.node_key, frame.attempt,
                                   *frame.timeout_budget, attempt_start);
}

template <stdexec::sender sender_t>
inline auto detail::invoke_runtime::invoke_session::make_async_timed_node_sender(
    sender_t &&sender, detail::runtime_state::invoke_outputs &outputs,
    const node_frame &frame,
    const std::chrono::steady_clock::time_point attempt_start) -> graph_sender {
  auto normalized = ::wh::compose::detail::normalize_graph_sender(
      std::forward<sender_t>(sender));
  if (!frame.timeout_budget.has_value()) {
    return ::wh::compose::detail::bridge_graph_sender(std::move(normalized));
  }

  const auto timeout_budget = *frame.timeout_budget;
  auto timeout_sender =
      exec::schedule_after(timeout_scheduler(), timeout_budget) |
      stdexec::then([&outputs, node_key = frame.cause.node_key,
                     attempt = frame.attempt, timeout_budget,
                     attempt_start]() mutable {
        return make_node_timeout_failure(outputs, node_key, attempt,
                                         timeout_budget, attempt_start);
      });
  return ::wh::compose::detail::bridge_graph_sender(
      exec::when_any(std::move(normalized), std::move(timeout_sender)));
}

template <typename retry_fn_t>
inline auto detail::invoke_runtime::invoke_session::run_sync_node_execution(
    const compiled_node &node, graph_value &input_value,
    wh::core::run_context &context, const graph_call_scope &bound_call_options,
    invoke_session *state, node_frame &frame, retry_fn_t retry_fn)
    -> wh::core::result<graph_value> {
  while (true) {
    bind_node_runtime_call_options(frame, bound_call_options, state);
    const auto attempt_start = std::chrono::steady_clock::now();
    auto executed =
        run_compiled_sync_node(node, input_value, context, frame.node_runtime);
    executed = apply_node_timeout(state->invoke_state().outputs, frame,
                                  attempt_start, std::move(executed));
    if (!executed.has_error() || frame.attempt >= frame.retry_budget) {
      return executed;
    }
    retry_fn(frame.node_id, frame.cause.step);
    ++frame.attempt;
  }
}

inline auto detail::invoke_runtime::invoke_session::make_async_node_attempt_sender(
    const compiled_node &node, graph_value &input_value,
    wh::core::run_context &context, const graph_call_scope &bound_call_options,
    invoke_session *state, node_frame &frame) -> graph_sender {
  bind_node_runtime_call_options(frame, bound_call_options, state);
  const auto attempt_start = std::chrono::steady_clock::now();
  return make_async_timed_node_sender(
      run_compiled_async_node(node, input_value, context, frame.node_runtime),
      state->invoke_state().outputs, frame, attempt_start);
}

} // namespace wh::compose
