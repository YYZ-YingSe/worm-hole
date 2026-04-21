// Defines invoke-session node stage pipeline helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::invoke_session::make_input_attempt(const std::uint32_t node_id,
                                                                       const std::size_t step)
    -> wh::core::result<attempt_id> {
  const auto &index = compiled_graph_index();
  const auto *node = index.nodes_by_id[node_id];
  if (node == nullptr) {
    return wh::core::result<attempt_id>::failure(wh::core::errc::not_found);
  }

  const auto attempt = attempt_id{node_id};
  release_attempt(attempt);
  auto &slot_state = slot(attempt);
  slot_state.current_stage = invoke_stage::input;
  slot_state.node_id = node_id;
  slot_state.cause = graph_state_cause{
      .run_id = invoke_state().run_id,
      .step = step,
      .node_key = index.id_to_key[node_id],
  };
  slot_state.node = node;
  if (node_id < cache_state().resolved_state_handlers.size()) {
    slot_state.state_handlers = cache_state().resolved_state_handlers[node_id];
  }
  return attempt;
}

inline auto detail::invoke_runtime::invoke_session::store_attempt_input(const attempt_id attempt,
                                                                        graph_value input)
    -> wh::core::result<void> {
  auto &slot_state = slot(attempt);
  if (slot_state.node == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }
  if (!slot_state.input.has_value()) {
    slot_state.input.emplace();
  }
  slot_state.input->payload.emplace(std::move(input));
  return {};
}

inline auto detail::invoke_runtime::invoke_session::begin_state_pre(const attempt_id attempt)
    -> wh::core::result<state_step> {
  auto &slot_state = slot(attempt);
  auto &invoke = invoke_state();
  if (slot_state.node == nullptr) {
    return wh::core::result<state_step>::failure(wh::core::errc::not_found);
  }
  if (!slot_state.input.has_value() || !slot_state.input->payload.has_value()) {
    return wh::core::result<state_step>::failure(wh::core::errc::not_found);
  }
  auto &input = *slot_state.input->payload;

  const auto &current_node_key = node_key(slot_state.node_id);
  auto resume_interrupt = evaluate_resume_match(slot_state.node_id);
  if (resume_interrupt.has_error()) {
    return wh::core::result<state_step>::failure(resume_interrupt.error());
  }
  if (resume_interrupt.value().has_value()) {
    context_.interrupt_info =
        wh::compose::to_interrupt_context(std::move(resume_interrupt.value().value()));
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit, slot_state.node_id,
               slot_state.cause.step);
    pending_inputs_.store_input(slot_state.node_id, std::move(input));
    slot_state.input->payload.reset();
    request_freeze(false);
    return wh::core::result<state_step>::failure(wh::core::errc::canceled);
  }

  auto pre_interrupt = owner_->evaluate_interrupt_hook(context_, invoke.config.interrupt_pre_hook,
                                                       current_node_key, input);
  if (pre_interrupt.has_error()) {
    return wh::core::result<state_step>::failure(pre_interrupt.error());
  }
  if (pre_interrupt.value().has_value()) {
    context_.interrupt_info =
        wh::compose::to_interrupt_context(std::move(pre_interrupt.value().value()));
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit, slot_state.node_id,
               slot_state.cause.step);
    pending_inputs_.store_input(slot_state.node_id, std::move(input));
    slot_state.input->payload.reset();
    request_freeze(false);
    return wh::core::result<state_step>::failure(wh::core::errc::canceled);
  }

  auto node_local_state = detail::process_runtime::acquire_node_local_process_state(
      node_local_process_states_, slot_state.node_id, process_state_);
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

  slot_state.node_scope.path = runtime_node_path(slot_state.node_id);
  slot_state.node_scope.local_process_state = std::addressof(node_local_ref.value().get());

  const auto skip_pre_state_handlers =
      restore_skip_pre_handlers_ && pending_inputs_.restored_node(slot_state.node_id);
  if (!skip_pre_state_handlers) {
    if (detail::state_runtime::needs_async_phase(slot_state.state_handlers, input,
                                                 detail::state_runtime::state_phase::pre)) {
      auto async_input = std::move(input);
      slot_state.input->payload.reset();
      auto sender = owner_->apply_state_phase_async(
          context_, slot_state.state_handlers, detail::state_runtime::state_phase::pre,
          current_node_key, slot_state.cause, node_local_ref.value().get(), std::move(async_input),
          slot_state.node_scope.path, invoke.outputs, *invoke.work_scheduler);
      slot_state.current_stage = invoke_stage::pre_state;
      node_local_guard.dismiss();
      slot_state.node_local_scope = std::move(node_local_scope);
      return state_step{
          .attempt = attempt,
          .sender = std::move(sender),
      };
    }

    auto pre_state = owner_->apply_state_phase(
        context_, slot_state.state_handlers, detail::state_runtime::state_phase::pre,
        current_node_key, slot_state.cause, node_local_ref.value().get(), input,
        slot_state.node_scope.path, invoke.outputs);
    if (pre_state.has_error()) {
      owner_->publish_node_run_error(invoke.outputs, slot_state.node_scope.path, slot_state.node_id,
                                     pre_state.error(), "node pre-state handler failed");
      state_table_.update(slot_state.node_id, graph_node_lifecycle_state::failed, 1U,
                          pre_state.error());
      append_transition(slot_state.node_id, graph_state_transition_event{
                                                .kind = graph_state_transition_kind::node_fail,
                                                .cause = slot_state.cause,
                                                .lifecycle = graph_node_lifecycle_state::failed,
                                            });
      return wh::core::result<state_step>::failure(pre_state.error());
    }
  }

  node_local_guard.dismiss();
  slot_state.node_local_scope = std::move(node_local_scope);
  return state_step{
      .attempt = attempt,
      .sender = std::nullopt,
  };
}

inline auto
detail::invoke_runtime::invoke_session::prepare_execution_input(const attempt_id attempt)
    -> wh::core::result<state_step> {
  auto &slot_state = slot(attempt);
  if (!slot_state.input.has_value()) {
    return wh::core::result<state_step>::failure(wh::core::errc::not_found);
  }
  auto &input_state = *slot_state.input;
  if (!input_state.lowering.has_value()) {
    if (!input_state.payload.has_value()) {
      return wh::core::result<state_step>::failure(wh::core::errc::not_found);
    }
    return state_step{
        .attempt = attempt,
        .sender = std::nullopt,
    };
  }
  if (!input_state.payload.has_value()) {
    return wh::core::result<state_step>::failure(wh::core::errc::not_found);
  }

  auto input = std::move(*input_state.payload);
  input_state.payload.reset();
  auto *reader = wh::core::any_cast<graph_stream_reader>(&input);
  if (reader == nullptr) {
    return wh::core::result<state_step>::failure(wh::core::errc::type_mismatch);
  }

  auto lowering = std::move(*input_state.lowering);
  input_state.lowering.reset();
  slot_state.current_stage = invoke_stage::prepare;
  return state_step{
      .attempt = attempt,
      .sender = owner_->lower_reader(std::move(*reader), std::move(lowering), context_,
                                     *invoke_state().work_scheduler),
  };
}

inline auto detail::invoke_runtime::invoke_session::should_retain_input(
    const attempt_slot &slot_state) const noexcept -> bool {
  return invoke_state().retain_inputs || slot_state.retry_budget > 0U;
}

inline auto detail::invoke_runtime::invoke_session::finalize_node_attempt(const attempt_id attempt)
    -> wh::core::result<void> {
  auto &slot_state = slot(attempt);
  if (!slot_state.input.has_value() || !slot_state.input->payload.has_value()) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }
  auto &input = *slot_state.input->payload;
  state_table_.update(slot_state.node_id, graph_node_lifecycle_state::running, 0U, std::nullopt);
  append_transition(slot_state.node_id, graph_state_transition_event{
                                            .kind = graph_state_transition_kind::node_enter,
                                            .cause = slot_state.cause,
                                            .lifecycle = graph_node_lifecycle_state::running,
                                        });

  const auto node_retry_budget = owner_->resolve_node_retry_budget(slot_state.node_id);
  const auto node_timeout_budget = owner_->resolve_node_timeout_budget(slot_state.node_id);
  const auto effective_parallel_gate =
      std::min(max_parallel_nodes(), owner_->resolve_node_parallel_gate(slot_state.node_id));
  if (effective_parallel_gate == 0U) {
    const auto node_id = slot_state.node_id;
    const auto cause = slot_state.cause;
    release_attempt(attempt);
    state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                        wh::core::errc::contract_violation);
    append_transition(node_id, graph_state_transition_event{
                                   .kind = graph_state_transition_kind::node_fail,
                                   .cause = std::move(cause),
                                   .lifecycle = graph_node_lifecycle_state::failed,
                               });
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }

  slot_state.current_stage = invoke_stage::node;
  slot_state.retry_budget = node_retry_budget;
  slot_state.timeout_budget = node_timeout_budget;
  if (should_retain_input(slot_state)) {
    auto retained_input = slot_state.node->meta.input_contract == node_contract::stream
                              ? detail::fork_graph_reader_payload(input)
                              : fork_graph_value(input);
    if (retained_input.has_error()) {
      const auto code = retained_input.error() == wh::core::errc::not_supported
                            ? wh::core::errc::contract_violation
                            : retained_input.error();
      const auto node_id = slot_state.node_id;
      const auto cause = slot_state.cause;
      release_attempt(attempt);
      state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U, code);
      append_transition(node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::node_fail,
                                     .cause = std::move(cause),
                                     .lifecycle = graph_node_lifecycle_state::failed,
                                 });
      return wh::core::result<void>::failure(code);
    }
    pending_inputs_.store_input(slot_state.node_id, std::move(retained_input).value());
  }
  detail::node_runtime_access::reset(slot_state.runtime, effective_parallel_gate);
  return {};
}

inline auto detail::invoke_runtime::invoke_session::begin_state_post(const attempt_id attempt,
                                                                     graph_value &output)
    -> wh::core::result<std::optional<graph_sender>> {
  auto &slot_state = slot(attempt);
  auto &invoke = invoke_state();
  if (detail::state_runtime::needs_async_phase(slot_state.state_handlers, output,
                                               detail::state_runtime::state_phase::post)) {
    auto sender = owner_->apply_state_phase_async(
        context_, slot_state.state_handlers, detail::state_runtime::state_phase::post,
        slot_state.cause.node_key, slot_state.cause, *slot_state.node_scope.local_process_state,
        std::move(output), slot_state.node_scope.path, invoke.outputs, *invoke.work_scheduler);
    slot_state.current_stage = invoke_stage::post_state;
    return std::optional<graph_sender>{std::move(sender)};
  }

  auto post_state = owner_->apply_state_phase(
      context_, slot_state.state_handlers, detail::state_runtime::state_phase::post,
      slot_state.cause.node_key, slot_state.cause, *slot_state.node_scope.local_process_state,
      output, slot_state.node_scope.path, invoke.outputs);
  if (post_state.has_error()) {
    owner_->publish_node_run_error(invoke.outputs, slot_state.node_scope.path, slot_state.node_id,
                                   post_state.error(), "node post-state handler failed");
    state_table_.update(slot_state.node_id, graph_node_lifecycle_state::failed, 1U,
                        post_state.error());
    append_transition(slot_state.node_id, graph_state_transition_event{
                                              .kind = graph_state_transition_kind::node_fail,
                                              .cause = slot_state.cause,
                                              .lifecycle = graph_node_lifecycle_state::failed,
                                          });
    release_attempt(attempt);
    return wh::core::result<std::optional<graph_sender>>::failure(post_state.error());
  }
  return std::optional<graph_sender>{};
}

inline auto detail::invoke_runtime::invoke_session::fail_node_stage(const attempt_id attempt,
                                                                    const wh::core::error_code code,
                                                                    const std::string_view message)
    -> wh::core::result<void> {
  auto &slot_state = slot(attempt);
  owner_->publish_node_run_error(invoke_state().outputs, slot_state.node_scope.path,
                                 slot_state.node_id, code, message);
  state_table_.update(slot_state.node_id, graph_node_lifecycle_state::failed, 1U, code);
  append_transition(slot_state.node_id, graph_state_transition_event{
                                            .kind = graph_state_transition_kind::node_fail,
                                            .cause = slot_state.cause,
                                            .lifecycle = graph_node_lifecycle_state::failed,
                                        });
  release_attempt(attempt);
  return wh::core::result<void>::failure(code);
}

inline auto detail::invoke_runtime::invoke_session::bind_node_runtime_call_options(
    attempt_slot &slot, const graph_call_scope &bound_call_options, invoke_session *state) noexcept
    -> void {
  slot.node_scope.component_options =
      state->cache_state().resolved_component_options.empty()
          ? nullptr
          : std::addressof(state->cache_state().resolved_component_options[slot.node_id]);
  slot.node_scope.observation =
      state->cache_state().resolved_node_observations.empty()
          ? nullptr
          : std::addressof(state->cache_state().resolved_node_observations[slot.node_id]);
  slot.node_scope.trace = state->next_node_trace(slot.node_id);
  detail::node_runtime_access::bind_scope(slot.runtime, std::addressof(bound_call_options),
                                          std::addressof(slot.node_scope.path));
  detail::node_runtime_access::bind_runtime(
      slot.runtime, std::addressof(*state->invoke_state().control_scheduler),
      std::addressof(*state->invoke_state().work_scheduler), slot.node_scope.local_process_state,
      slot.node_scope.observation, std::addressof(slot.node_scope.trace));
  detail::node_runtime_access::bind_internal(
      slot.runtime, std::addressof(state->invoke_state().outputs), state->nested_graph_entry());
}

inline auto detail::invoke_runtime::invoke_session::start_nested_from_runtime(
    const void *state_ptr, const graph &nested_graph, wh::core::run_context &context,
    graph_value &input, const graph_call_scope *call_options, const node_path *path_prefix,
    graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs, const graph_node_trace *parent_trace)
    -> graph_sender {
  const auto *state = static_cast<const invoke_session *>(state_ptr);
  auto nested_call_scope =
      call_options != nullptr
          ? graph_call_scope{call_options->options(),
                             path_prefix != nullptr ? *path_prefix : node_path{}}
          : graph_call_scope{};
  if (parent_trace != nullptr) {
    auto trace = nested_call_scope.trace().value_or(graph_trace_context{});
    if (trace.trace_id.empty() && !parent_trace->trace_id.empty()) {
      trace.trace_id = std::string{parent_trace->trace_id};
    }
    trace.parent_span_id = parent_trace->span_id;
    nested_call_scope = nested_call_scope.with_trace(std::move(trace));
  }
  return detail::start_scoped_graph(nested_graph, context, input, std::addressof(nested_call_scope),
                                    path_prefix, parent_process_state, nested_outputs,
                                    *state->invoke_state().control_scheduler,
                                    *state->invoke_state().work_scheduler, state);
}

inline auto detail::invoke_runtime::invoke_session::nested_graph_entry() const noexcept
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
    detail::runtime_state::invoke_outputs &outputs, const std::string_view node_key,
    const std::size_t attempt, const std::chrono::milliseconds timeout_budget,
    const std::chrono::steady_clock::time_point attempt_start) -> wh::core::result<graph_value> {
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
    detail::runtime_state::invoke_outputs &outputs, const attempt_slot &slot,
    const std::chrono::steady_clock::time_point attempt_start,
    wh::core::result<graph_value> executed) -> wh::core::result<graph_value> {
  if (!slot.timeout_budget.has_value()) {
    return executed;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - attempt_start);
  if (elapsed <= *slot.timeout_budget) {
    return executed;
  }
  return make_node_timeout_failure(outputs, slot.cause.node_key, slot.attempt, *slot.timeout_budget,
                                   attempt_start);
}

template <stdexec::sender sender_t>
inline auto detail::invoke_runtime::invoke_session::make_async_timed_node_sender(
    sender_t &&sender, detail::runtime_state::invoke_outputs &outputs, const attempt_slot &slot,
    const std::chrono::steady_clock::time_point attempt_start) -> graph_sender {
  auto normalized = ::wh::compose::detail::normalize_graph_sender(std::forward<sender_t>(sender));
  if (!slot.timeout_budget.has_value()) {
    return ::wh::compose::detail::bridge_graph_sender(std::move(normalized));
  }

  const auto timeout_budget = *slot.timeout_budget;
  auto timeout_sender =
      exec::schedule_after(timeout_scheduler(), timeout_budget) |
      stdexec::then([&outputs, node_key = slot.cause.node_key, attempt = slot.attempt,
                     timeout_budget, attempt_start]() mutable {
        return make_node_timeout_failure(outputs, node_key, attempt, timeout_budget, attempt_start);
      });
  return ::wh::compose::detail::bridge_graph_sender(
      exec::when_any(std::move(normalized), std::move(timeout_sender)));
}

inline auto detail::invoke_runtime::invoke_session::run_sync_node_execution(
    const compiled_node &node, graph_value &input_value, wh::core::run_context &context,
    const graph_call_scope &bound_call_options, invoke_session *state, attempt_slot &slot,
    const bool apply_timeout_after_execution) -> wh::core::result<graph_value> {
  bind_node_runtime_call_options(slot, bound_call_options, state);
  const auto attempt_start = std::chrono::steady_clock::now();
  auto executed = run_compiled_sync_node(node, input_value, context, slot.runtime);
  if (!apply_timeout_after_execution) {
    return executed;
  }
  return apply_node_timeout(state->invoke_state().outputs, slot, attempt_start,
                            std::move(executed));
}

inline auto detail::invoke_runtime::invoke_session::make_sync_node_attempt_sender(
    const compiled_node &node, graph_value &input_value, wh::core::run_context &context,
    const graph_call_scope &bound_call_options, invoke_session *state, attempt_slot &slot)
    -> graph_sender {
  const auto attempt_start = std::chrono::steady_clock::now();
  auto sender =
      stdexec::schedule(*state->invoke_state().work_scheduler) |
      stdexec::then([&node, &input_value, &context, &bound_call_options, state, &slot]() mutable {
        return run_sync_node_execution(node, input_value, context, bound_call_options, state, slot,
                                       false);
      });
  return make_async_timed_node_sender(std::move(sender), state->invoke_state().outputs, slot,
                                      attempt_start);
}

inline auto detail::invoke_runtime::invoke_session::make_async_node_attempt_sender(
    const compiled_node &node, graph_value &input_value, wh::core::run_context &context,
    const graph_call_scope &bound_call_options, invoke_session *state, attempt_slot &slot)
    -> graph_sender {
  bind_node_runtime_call_options(slot, bound_call_options, state);
  const auto attempt_start = std::chrono::steady_clock::now();
  return make_async_timed_node_sender(
      stdexec::starts_on(*state->invoke_state().work_scheduler,
                         run_compiled_async_node(node, input_value, context, slot.runtime)),
      state->invoke_state().outputs, slot, attempt_start);
}

} // namespace wh::compose
