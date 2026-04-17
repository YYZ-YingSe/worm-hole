// Defines invoke-session lifecycle and runtime-control helpers.
#pragma once

#include <charconv>
#include <cstdio>

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

namespace detail {

[[nodiscard]] inline auto
format_trace_token(const std::string_view prefix, const std::uint64_t run_id,
                   const std::optional<std::uint64_t> sequence = std::nullopt)
    -> std::string {
  std::array<char, 64U> buffer{};
  char *cursor = buffer.data();
  for (const char value : prefix) {
    *cursor++ = value;
  }
  auto [run_end, run_error] =
      std::to_chars(cursor, buffer.data() + buffer.size(), run_id);
  if (run_error != std::errc{}) {
    std::string fallback{prefix};
    fallback.append(std::to_string(run_id));
    if (sequence.has_value()) {
      fallback.push_back('.');
      fallback.append(std::to_string(*sequence));
    }
    return fallback;
  }
  cursor = run_end;
  if (sequence.has_value()) {
    *cursor++ = '.';
    auto [sequence_end, sequence_error] =
        std::to_chars(cursor, buffer.data() + buffer.size(), *sequence);
    if (sequence_error != std::errc{}) {
      std::string fallback{buffer.data(),
                           static_cast<std::size_t>(cursor - buffer.data())};
      fallback.append(std::to_string(*sequence));
      return fallback;
    }
    cursor = sequence_end;
  }
  return std::string{buffer.data(),
                     static_cast<std::size_t>(cursor - buffer.data())};
}

} // namespace detail

inline auto detail::invoke_runtime::invoke_session::initialize_runtime_node_caches()
    -> void {
  auto &cache = cache_state();
  const auto node_count = compiled_graph_index().nodes_by_id.size();
  cache.runtime_node_paths.clear();
  cache.runtime_node_paths.resize(node_count);
  cache.runtime_stream_scopes.clear();
  if (cache.collect_transition_log || cache.emit_state_snapshot_events ||
      cache.emit_state_delta_events || cache.emit_runtime_message_events ||
      cache.emit_custom_events) {
    cache.runtime_stream_scopes.resize(node_count);
  }
  cache.runtime_node_execution_addresses.clear();
  cache.runtime_node_execution_addresses.resize(node_count);
}

inline auto
detail::invoke_runtime::invoke_session::initialize_resolved_component_options()
    -> void {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  const auto node_count = compiled_graph_index().nodes_by_id.size();
  cache.resolved_component_options.clear();
  cache.resolved_component_options.resize(node_count);
  if (!cache.has_component_option_overrides) {
    return;
  }

  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    cache.resolved_component_options[node_id] =
        resolve_graph_component_option_map(
            invoke.bound_call_scope,
            owner_->make_node_designation_path(node_id));
  }
}

inline auto
detail::invoke_runtime::invoke_session::initialize_resolved_node_observations()
    -> void {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  const auto node_count = compiled_graph_index().nodes_by_id.size();
  cache.resolved_node_observations.clear();
  cache.resolved_node_observations.resize(node_count);

  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    const auto *node = compiled_graph_index().nodes_by_id[node_id];
    if (node == nullptr) {
      continue;
    }

    auto &resolved = cache.resolved_node_observations[node_id];
    const auto &defaults = node->meta.options.observation;
    resolved.callbacks_enabled = defaults.callbacks_enabled;
    resolved.local_callbacks = defaults.local_callbacks;
    if (!defaults.allow_invoke_override) {
      continue;
    }

    const auto path = owner_->make_node_designation_path(node_id);
    for (const auto &rule :
         invoke.bound_call_scope.options().node_observations) {
      if (!matches_node_observation(invoke.bound_call_scope, path, rule)) {
        continue;
      }
      if (rule.callbacks_enabled.has_value()) {
        resolved.callbacks_enabled = *rule.callbacks_enabled;
      }
      if (!rule.local_callbacks.has_value()) {
        continue;
      }
      resolved.local_callbacks.insert(resolved.local_callbacks.end(),
                                      rule.local_callbacks->begin(),
                                      rule.local_callbacks->end());
    }
  }
}

inline auto
detail::invoke_runtime::invoke_session::initialize_resolved_state_handlers()
    -> wh::core::result<void> {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  const auto node_count = compiled_graph_index().nodes_by_id.size();
  cache.resolved_state_handlers.clear();
  cache.resolved_state_handlers.resize(node_count, nullptr);

  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    const auto *node = compiled_graph_index().nodes_by_id[node_id];
    if (node == nullptr) {
      continue;
    }
    auto handlers = detail::state_runtime::resolve_node_state_handlers(
        invoke.config.state_handlers, compiled_graph_index().id_to_key[node_id],
        node->meta.options);
    if (handlers.has_error()) {
      return wh::core::result<void>::failure(handlers.error());
    }
    cache.resolved_state_handlers[node_id] = handlers.value();
  }
  return {};
}

inline auto detail::invoke_runtime::invoke_session::initialize_trace_state()
    -> void {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  cache.trace = detail::runtime_state::graph_trace_state{};
  if (invoke.bound_call_scope.trace().has_value()) {
    cache.trace.trace_id = invoke.bound_call_scope.trace()->trace_id;
    cache.trace.parent_span_id =
        invoke.bound_call_scope.trace()->parent_span_id;
  }
  if (cache.trace.trace_id.empty()) {
    cache.trace.trace_id = detail::format_trace_token("g", invoke.run_id);
  }
  cache.trace.graph_span_id =
      detail::format_trace_token("gs", invoke.run_id, 0U);
  cache.trace.next_span_sequence = 1U;
}

inline auto
detail::invoke_runtime::invoke_session::next_node_trace(const std::uint32_t node_id)
    -> graph_node_trace {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  graph_node_trace trace{};
  trace.trace_id = cache.trace.trace_id;
  trace.span_id = detail::format_trace_token("ns", invoke.run_id,
                                             cache.trace.next_span_sequence++);
  trace.parent_span_id = cache.trace.graph_span_id;
  trace.path = std::addressof(runtime_node_execution_address(node_id));
  return trace;
}

inline auto detail::invoke_runtime::invoke_session::runtime_node_path(
    const std::uint32_t node_id) -> const node_path & {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  wh_precondition(node_id < cache.runtime_node_paths.size());
  auto &path = cache.runtime_node_paths[node_id];
  if (path.empty()) {
    path = owner_->make_runtime_node_path(invoke.path_prefix, node_id);
  }
  return path;
}

inline auto detail::invoke_runtime::invoke_session::runtime_stream_scope(
    const std::uint32_t node_id) -> const graph_event_scope & {
  auto &cache = cache_state();
  const auto stream_node_count = node_count();
  if (cache.runtime_stream_scopes.size() != stream_node_count) {
    cache.runtime_stream_scopes.resize(stream_node_count);
  }
  wh_precondition(node_id < cache.runtime_stream_scopes.size());
  auto &scope = cache.runtime_stream_scopes[node_id];
  if (scope.node.empty()) {
    scope = make_graph_event_scope(graph_options().name, node_key(node_id),
                                   runtime_node_path(node_id));
  }
  return scope;
}

inline auto detail::invoke_runtime::invoke_session::runtime_node_execution_address(
    const std::uint32_t node_id) -> const wh::core::address & {
  auto &cache = cache_state();
  wh_precondition(node_id < cache.runtime_node_execution_addresses.size());
  auto &location = cache.runtime_node_execution_addresses[node_id];
  if (location.empty()) {
    location = owner_->make_node_execution_address(runtime_node_path(node_id));
  }
  return location;
}

inline auto detail::invoke_runtime::invoke_session::transition_log() noexcept
    -> graph_transition_log & {
  return invoke_state().outputs.transition_log;
}

inline auto detail::invoke_runtime::invoke_session::initialize(
    graph_value &&input, graph_call_scope call_scope) -> void {
  auto &invoke = invoke_state();
  auto &cache = cache_state();
  auto &interrupt = interrupt_state();
  const auto &index = compiled_graph_index();
  if (index.nodes_by_id.empty()) {
    owner_->publish_graph_run_error(
        invoke.outputs, std::nullopt, {}, compose_error_phase::execute,
        wh::core::errc::contract_violation, "compiled runtime index is empty");
    init_error_ = wh::core::errc::contract_violation;
    return;
  }
  if (invoke.parent_state != nullptr) {
    invoke.services = invoke.parent_state->invoke_state().services;
    invoke.config = invoke.parent_state->invoke_state().config;
    invoke.forwarded_checkpoints =
        invoke.parent_state->invoke_state().forwarded_checkpoints;
  } else {
    invoke.config.state_handlers =
        invoke.services != nullptr ? invoke.services->state_handlers : nullptr;
    invoke.config.checkpoint_store = invoke.services != nullptr
                                         ? invoke.services->checkpoint.store
                                         : nullptr;
    invoke.config.checkpoint_backend = invoke.services != nullptr
                                           ? invoke.services->checkpoint.backend
                                           : nullptr;
    invoke.config.checkpoint_stream_codecs =
        invoke.services != nullptr ? invoke.services->checkpoint.stream_codecs
                                   : nullptr;
    invoke.config.checkpoint_serializer =
        invoke.services != nullptr ? invoke.services->checkpoint.serializer
                                   : nullptr;
    invoke.config.checkpoint_load = invoke.controls.checkpoint.load;
    invoke.config.checkpoint_save = invoke.controls.checkpoint.save;
    invoke.config.checkpoint_before_load =
        invoke.controls.checkpoint.before_load;
    invoke.config.checkpoint_before_load_nodes =
        invoke.controls.checkpoint.before_load_nodes;
    invoke.config.checkpoint_after_load = invoke.controls.checkpoint.after_load;
    invoke.config.checkpoint_after_load_nodes =
        invoke.controls.checkpoint.after_load_nodes;
    invoke.config.checkpoint_before_save =
        invoke.controls.checkpoint.before_save;
    invoke.config.checkpoint_before_save_nodes =
        invoke.controls.checkpoint.before_save_nodes;
    invoke.config.checkpoint_after_save = invoke.controls.checkpoint.after_save;
    invoke.config.checkpoint_after_save_nodes =
        invoke.controls.checkpoint.after_save_nodes;
    invoke.config.interrupt_pre_hook = invoke.controls.interrupt.pre_hook;
    invoke.config.interrupt_post_hook = invoke.controls.interrupt.post_hook;
    invoke.config.resume_contexts = invoke.controls.resume.contexts;
    invoke.config.subgraph_interrupt_signals =
        invoke.controls.interrupt.subgraph_signals;
    invoke.config.resume_decision = invoke.controls.resume.decision;
    invoke.config.batch_resume_items = invoke.controls.resume.batch_items;
    invoke.config.reinterrupt_unmatched =
        invoke.controls.resume.reinterrupt_unmatched;
    invoke.config.pregel_max_steps_override =
        invoke.controls.schedule.pregel_max_steps;
    if (invoke.controls.schedule.branch_merge.has_value()) {
      invoke.config.branch_merge = *invoke.controls.schedule.branch_merge;
    }
    invoke.owned_forwarded_checkpoints =
        std::move(invoke.controls.checkpoint.forwarded_once);
    invoke.forwarded_checkpoints =
        std::addressof(invoke.owned_forwarded_checkpoints);
  }

  auto checkpoint_config =
      detail::checkpoint_runtime::validate_runtime_configuration(
          invoke.config, invoke.outputs);
  if (checkpoint_config.has_error()) {
    owner_->publish_graph_run_error(
        invoke.outputs, std::nullopt, {}, compose_error_phase::checkpoint,
        checkpoint_config.error(), "checkpoint runtime configuration invalid");
    init_error_ = checkpoint_config.error();
    return;
  }
  auto call_options_validated =
      owner_->validate_call_scope_for_runtime(call_scope);
  if (call_options_validated.has_error()) {
    owner_->publish_graph_run_error(
        invoke.outputs, std::nullopt, {}, compose_error_phase::schedule,
        call_options_validated.error(), "call options validation failed");
    init_error_ = call_options_validated.error();
    return;
  }
  const bool has_checkpoint_backend =
      invoke.config.checkpoint_store != nullptr ||
      invoke.config.checkpoint_backend != nullptr;
  invoke.retain_inputs =
      has_checkpoint_backend || context_.resume_info.has_value() ||
      invoke.config.interrupt_pre_hook || invoke.config.interrupt_post_hook ||
      invoke.config.resume_decision.has_value() ||
      !invoke.config.batch_resume_items.empty() ||
      !invoke.config.resume_contexts.empty() ||
      !invoke.config.subgraph_interrupt_signals.empty();

  const auto total_nodes = index.nodes_by_id.size();
  state_table_.reset(index.id_to_key);
  pending_inputs_.reset(total_nodes);

  detail::process_runtime::bind_parent_process_state(
      process_state_, invoke.parent_process_state);
  transition_log().clear();
  entry_input_.reset();
  restore_plan_.reset();
  invoke.start_entry_selection.reset();
  restore_skip_pre_handlers_ = false;
  invoke.step_count = 0U;

  invoke.run_id = owner_->next_invoke_run_id();
  const auto restore_scope =
      invoke.parent_process_state == nullptr
          ? detail::checkpoint_runtime::restore_scope::full
          : detail::checkpoint_runtime::restore_scope::forwarded_only;
  auto prepared_restore = owner_->prepare_restore_checkpoint(
      context_, invoke.config, restore_scope, invoke.path_prefix,
      invoke.outputs, *invoke.forwarded_checkpoints);
  if (prepared_restore.has_error()) {
    init_error_ = prepared_restore.error();
    return;
  }
  restore_plan_ = std::move(prepared_restore).value();
  auto runtime_resume =
      owner_->apply_runtime_resume_controls(context_, invoke.config);
  if (runtime_resume.has_error()) {
    init_error_ = runtime_resume.error();
    return;
  }
  auto resume_state_overrides =
      detail::interrupt_runtime::apply_resume_data_state_overrides(
          context_, state_table_);
  if (resume_state_overrides.has_error()) {
    init_error_ = resume_state_overrides.error();
    return;
  }

  auto step_budget = owner_->resolve_step_budget(invoke.config, call_scope);
  if (step_budget.has_error()) {
    owner_->publish_graph_run_error(
        invoke.outputs, std::nullopt, {}, compose_error_phase::schedule,
        step_budget.error(), "step budget resolution failed");
    init_error_ = step_budget.error();
    return;
  }
  invoke.step_budget = step_budget.value();

  invoke.bound_call_scope = std::move(call_scope);
  cache.has_component_option_overrides =
      !invoke.bound_call_scope.component_defaults().empty() ||
      !invoke.bound_call_scope.options().component_overrides.empty();
  initialize_resolved_component_options();
  initialize_resolved_node_observations();
  auto resolved_state_handlers = initialize_resolved_state_handlers();
  if (resolved_state_handlers.has_error()) {
    owner_->publish_graph_run_error(
        invoke.outputs, std::nullopt, {}, compose_error_phase::execute,
        resolved_state_handlers.error(), "state handler resolution failed");
    init_error_ = resolved_state_handlers.error();
    return;
  }
  initialize_trace_state();
  cache.emit_debug_events =
      should_emit_graph_debug_event(invoke.bound_call_scope);
  cache.collect_transition_log =
      invoke.bound_call_scope.record_transition_log();
  invoke.outputs.publish_transition_log = cache.collect_transition_log;
  cache.emit_state_snapshot_events = has_graph_stream_subscription(
      invoke.bound_call_scope, graph_stream_channel_kind::state_snapshot);
  cache.emit_state_delta_events = has_graph_stream_subscription(
      invoke.bound_call_scope, graph_stream_channel_kind::state_delta);
  cache.emit_runtime_message_events = has_graph_stream_subscription(
      invoke.bound_call_scope, graph_stream_channel_kind::message);
  cache.emit_custom_events = std::ranges::any_of(
      invoke.bound_call_scope.options().stream_subscriptions,
      [](const graph_stream_subscription &subscription) {
        return subscription.enabled &&
               subscription.kind == graph_stream_channel_kind::custom &&
               !subscription.custom_channel.empty();
      });
  io_storage_.reset(index.nodes_by_id.size(), index.indexed_edges.size());
  node_local_process_states_.clear();
  node_local_process_states_.resize(index.nodes_by_id.size());
  initialize_runtime_node_caches();
  invoke.outputs.completed_node_keys.reserve(total_nodes);
  if (cache.collect_transition_log) {
    transition_log().reserve(total_nodes * 4U);
  }
  interrupt.policy = resolve_external_interrupt_policy(invoke.bound_call_scope);
  interrupt.policy_latch = {};
  entry_input_ = std::move(input);
}

inline auto detail::invoke_runtime::invoke_session::initialize_start_entry(
    graph_value input) -> wh::core::result<void> {
  auto &invoke = invoke_state();
  const auto &index = compiled_graph_index();
  const auto start_node_id = index.start_id;
  const auto *start_node = start_node_id < index.nodes_by_id.size()
                               ? index.nodes_by_id[start_node_id]
                               : nullptr;
  if (start_node == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }

  const auto fail_start =
      [this, &invoke, start_node_id](const wh::core::error_code code)
      -> wh::core::result<void> {
    state_table_.update(start_node_id, graph_node_lifecycle_state::failed, 1U,
                        code);
    append_transition(start_node_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause =
                              graph_state_cause{
                                  .run_id = invoke.run_id,
                                  .step = 0U,
                                  .node_key = std::string{graph_start_node_key},
                              },
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    return wh::core::result<void>::failure(code);
  };

  const auto start_contract = start_node->meta.output_contract;
  auto start_branch = wh::core::result<
      std::optional<std::vector<std::uint32_t>>>::failure(
      wh::core::errc::internal_error);

  if (start_contract == node_contract::value) {
    start_branch = owner_->evaluate_value_branch_indexed(
        start_node_id, input, context_, invoke.bound_call_scope);
    if (start_branch.has_error()) {
      return fail_start(start_branch.error());
    }

    if (invoke.retain_inputs) {
      auto start_pending_input = fork_graph_value(input);
      if (start_pending_input.has_error()) {
        const auto code = start_pending_input.error() == wh::core::errc::not_supported
                              ? wh::core::errc::contract_violation
                              : start_pending_input.error();
        return fail_start(code);
      }
      pending_inputs_.store_input(start_node_id, std::move(start_pending_input).value());
    }

    auto committed_start = owner_->commit_value_output(
        start_node_id, io_storage_, std::move(input), start_branch.value(),
        context_);
    if (committed_start.has_error()) {
      return fail_start(committed_start.error());
    }
  } else {
    graph_stream_reader start_output_reader{};
    if (invoke.retain_inputs) {
      auto start_pending_input = std::move(input);
      auto start_output = detail::fork_graph_reader_payload(start_pending_input);
      if (start_output.has_error()) {
        return fail_start(start_output.error());
      }
      auto *reader =
          wh::core::any_cast<graph_stream_reader>(&start_output.value());
      if (reader == nullptr) {
        return fail_start(wh::core::errc::type_mismatch);
      }
      start_output_reader = std::move(*reader);
      pending_inputs_.store_input(start_node_id, std::move(start_pending_input));
    } else {
      auto *reader = wh::core::any_cast<graph_stream_reader>(&input);
      if (reader == nullptr) {
        return fail_start(wh::core::errc::type_mismatch);
      }
      start_output_reader = std::move(*reader);
    }

    start_branch = owner_->evaluate_stream_branch_indexed(
        start_node_id, start_output_reader, context_, invoke.bound_call_scope);
    if (start_branch.has_error()) {
      return fail_start(start_branch.error());
    }

    auto committed_start = owner_->commit_stream_output(
        start_node_id, io_storage_, std::move(start_output_reader),
        start_branch.value());
    if (committed_start.has_error()) {
      return fail_start(committed_start.error());
    }
  }

  state_table_.update(start_node_id, graph_node_lifecycle_state::completed, 0U,
                      std::nullopt);
  append_transition(start_node_id,
                    graph_state_transition_event{
                        .kind = graph_state_transition_kind::route_commit,
                        .cause =
                            graph_state_cause{
                                .run_id = invoke.run_id,
                                .step = 0U,
                                .node_key = std::string{graph_start_node_key},
                            },
                        .lifecycle = graph_node_lifecycle_state::completed,
                    });
  invoke.start_entry_selection = std::move(start_branch).value();
  return {};
}

inline auto
detail::invoke_runtime::invoke_session::capture_common_checkpoint_state()
    -> wh::core::result<checkpoint_state> {
  checkpoint_state checkpoint{};
  checkpoint.checkpoint_id = graph_options().name;
  checkpoint.restore_shape = core().restore_shape_;
  checkpoint.runtime.step_count = invoke_state().step_count;
  checkpoint.runtime.lifecycle = state_table_.states();

  auto owned_resume_snapshot =
      context_.resume_info.has_value()
          ? wh::core::into_owned(*context_.resume_info)
          : wh::core::result<wh::core::resume_state>{wh::core::resume_state{}};
  if (owned_resume_snapshot.has_error()) {
    return wh::core::result<checkpoint_state>::failure(
        owned_resume_snapshot.error());
  }
  checkpoint.resume_snapshot = std::move(owned_resume_snapshot).value();

  if (context_.interrupt_info.has_value()) {
    auto reinterrupt_signal =
        wh::compose::to_reinterrupt_signal(*context_.interrupt_info);
    if (reinterrupt_signal.has_error()) {
      return wh::core::result<checkpoint_state>::failure(
          reinterrupt_signal.error());
    }
    auto interrupt_snapshot =
        wh::core::flatten_interrupt_signals(std::vector<wh::core::interrupt_signal>{
            std::move(reinterrupt_signal).value()});
    if (interrupt_snapshot.has_error()) {
      return wh::core::result<checkpoint_state>::failure(
          interrupt_snapshot.error());
    }
    checkpoint.interrupt_snapshot = std::move(interrupt_snapshot).value();
  }
  return checkpoint;
}

inline auto
detail::invoke_runtime::invoke_session::immediate_success(graph_value value)
    -> graph_sender {
  publish_runtime_outputs();
  return detail::ready_graph_sender(
      wh::core::result<graph_value>{std::move(value)});
}

inline auto detail::invoke_runtime::invoke_session::immediate_failure(
    const wh::core::error_code code) -> graph_sender {
  publish_runtime_outputs();
  return detail::failure_graph_sender(code);
}

inline auto detail::invoke_runtime::invoke_session::publish_runtime_outputs()
    -> void {
  auto &invoke = invoke_state();
  if (invoke.forwarded_checkpoints != nullptr &&
      invoke.outputs.remaining_forwarded_checkpoint_keys.empty()) {
    invoke.outputs.remaining_forwarded_checkpoint_keys.reserve(
        invoke.forwarded_checkpoints->size());
    for (const auto &entry : *invoke.forwarded_checkpoints) {
      invoke.outputs.remaining_forwarded_checkpoint_keys.push_back(entry.first);
    }
    std::sort(invoke.outputs.remaining_forwarded_checkpoint_keys.begin(),
              invoke.outputs.remaining_forwarded_checkpoint_keys.end());
  }
  if (invoke.nested_outputs != nullptr) {
    detail::runtime_state::merge_nested_outputs(*invoke.nested_outputs,
                                                std::move(invoke.outputs));
    return;
  }
  if (invoke.published_outputs != nullptr) {
    invoke.published_outputs->remaining_forwarded_checkpoint_keys =
        std::move(invoke.outputs.remaining_forwarded_checkpoint_keys);
    detail::runtime_state::merge_nested_outputs(*invoke.published_outputs,
                                                std::move(invoke.outputs));
  }
}

inline auto detail::invoke_runtime::invoke_session::emit_debug(
    const graph_debug_stream_event::decision_kind decision,
    const std::uint32_t node_id, const std::size_t step) -> void {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  if (!cache.emit_debug_events) {
    return;
  }
  owner_->emit_debug_stream_event(context_, invoke.outputs,
                                  invoke.bound_call_scope, decision, node_id,
                                  runtime_node_path(node_id), step);
}

inline auto detail::invoke_runtime::invoke_session::append_transition(
    const graph_state_transition_event &event) -> void {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  const auto &index = compiled_graph_index();
  if (!cache.collect_transition_log && !cache.emit_state_snapshot_events &&
      !cache.emit_state_delta_events && !cache.emit_runtime_message_events &&
      !cache.emit_custom_events) {
    return;
  }
  const auto node_id_iter = index.key_to_id.find(event.cause.node_key);
  if (node_id_iter == index.key_to_id.end()) {
    detail::stream_runtime::append_state_transition(
        transition_log(), invoke.outputs, invoke.bound_call_scope, event,
        owner_->make_stream_scope(event.cause.node_key),
        cache.collect_transition_log, cache.emit_state_snapshot_events,
        cache.emit_state_delta_events, cache.emit_runtime_message_events,
        cache.emit_custom_events);
    return;
  }
  detail::stream_runtime::append_state_transition(
      transition_log(), invoke.outputs, invoke.bound_call_scope, event,
      runtime_stream_scope(node_id_iter->second), cache.collect_transition_log,
      cache.emit_state_snapshot_events, cache.emit_state_delta_events,
      cache.emit_runtime_message_events, cache.emit_custom_events);
}

inline auto detail::invoke_runtime::invoke_session::append_transition(
    graph_state_transition_event &&event) -> void {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  const auto &index = compiled_graph_index();
  if (!cache.collect_transition_log && !cache.emit_state_snapshot_events &&
      !cache.emit_state_delta_events && !cache.emit_runtime_message_events &&
      !cache.emit_custom_events) {
    return;
  }
  const auto node_id_iter = index.key_to_id.find(event.cause.node_key);
  if (node_id_iter == index.key_to_id.end()) {
    const auto scope = owner_->make_stream_scope(event.cause.node_key);
    detail::stream_runtime::append_state_transition(
        transition_log(), invoke.outputs, invoke.bound_call_scope,
        std::move(event), scope, cache.collect_transition_log,
        cache.emit_state_snapshot_events, cache.emit_state_delta_events,
        cache.emit_runtime_message_events, cache.emit_custom_events);
    return;
  }
  detail::stream_runtime::append_state_transition(
      transition_log(), invoke.outputs, invoke.bound_call_scope,
      std::move(event), runtime_stream_scope(node_id_iter->second),
      cache.collect_transition_log, cache.emit_state_snapshot_events,
      cache.emit_state_delta_events, cache.emit_runtime_message_events,
      cache.emit_custom_events);
}

inline auto detail::invoke_runtime::invoke_session::append_transition(
    const std::uint32_t node_id, const graph_state_transition_event &event)
    -> void {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  if (!cache.collect_transition_log && !cache.emit_state_snapshot_events &&
      !cache.emit_state_delta_events && !cache.emit_runtime_message_events &&
      !cache.emit_custom_events) {
    return;
  }
  detail::stream_runtime::append_state_transition(
      transition_log(), invoke.outputs, invoke.bound_call_scope, event,
      runtime_stream_scope(node_id), cache.collect_transition_log,
      cache.emit_state_snapshot_events, cache.emit_state_delta_events,
      cache.emit_runtime_message_events, cache.emit_custom_events);
}

inline auto detail::invoke_runtime::invoke_session::append_transition(
    const std::uint32_t node_id, graph_state_transition_event &&event) -> void {
  auto &cache = cache_state();
  auto &invoke = invoke_state();
  if (!cache.collect_transition_log && !cache.emit_state_snapshot_events &&
      !cache.emit_state_delta_events && !cache.emit_runtime_message_events &&
      !cache.emit_custom_events) {
    return;
  }
  detail::stream_runtime::append_state_transition(
      transition_log(), invoke.outputs, invoke.bound_call_scope,
      std::move(event), runtime_stream_scope(node_id),
      cache.collect_transition_log, cache.emit_state_snapshot_events,
      cache.emit_state_delta_events, cache.emit_runtime_message_events,
      cache.emit_custom_events);
}

inline auto detail::invoke_runtime::invoke_session::evaluate_resume_match(
    const std::uint32_t node_id)
    -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
  if (!context_.resume_info.has_value()) {
    return std::optional<wh::core::interrupt_signal>{};
  }
  const auto &location = runtime_node_execution_address(node_id);
  const auto match =
      classify_resume_target_match(*context_.resume_info, location);
  if (!match.in_resume_flow) {
    return std::optional<wh::core::interrupt_signal>{};
  }
  if (!match.should_reinterrupt) {
    if (match.match_kind == resume_target_match_kind::exact) {
      emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
                 node_id, 0U);
    }
    return std::optional<wh::core::interrupt_signal>{};
  }

  auto signal =
      make_interrupt_signal(make_interrupt_id(), location, std::monostate{});
  if (context_.interrupt_info.has_value() &&
      !context_.interrupt_info->interrupt_id.empty()) {
    signal.interrupt_id = context_.interrupt_info->interrupt_id;
  }
  return std::optional<wh::core::interrupt_signal>{std::move(signal)};
}

inline auto detail::invoke_runtime::invoke_session::control_slot_id() const noexcept
    -> std::uint32_t {
  return static_cast<std::uint32_t>(node_count());
}

inline auto detail::invoke_runtime::invoke_session::request_freeze(
    const bool external_interrupt) noexcept -> void {
  auto &interrupt = interrupt_state();
  auto &invoke = invoke_state();
  interrupt.freeze_requested = true;
  interrupt.freeze_external = external_interrupt;
  if (external_interrupt) {
    invoke.outputs.external_interrupt_resolution =
        detail::interrupt_runtime::resolve_external_resolution_kind(
            *detail::interrupt_runtime::freeze_external_policy_from_latch(
                 interrupt.policy_latch, interrupt.policy)
                 .value());
  }
}

inline auto detail::invoke_runtime::invoke_session::freeze_requested() const noexcept
    -> bool {
  return interrupt_state().freeze_requested;
}

inline auto detail::invoke_runtime::invoke_session::freeze_external() const noexcept
    -> bool {
  return interrupt_state().freeze_external;
}

template <typename persist_fn_t>
inline auto detail::invoke_runtime::invoke_session::make_freeze_sender(
    graph_sender capture_sender, const bool external_interrupt,
    persist_fn_t &&persist)
    -> graph_sender {
  auto &interrupt = interrupt_state();
  auto &invoke = invoke_state();
  const auto *policy =
      detail::interrupt_runtime::freeze_external_policy_from_latch(
          interrupt.policy_latch, interrupt.policy)
          .value();
  if (external_interrupt) {
    invoke.outputs.external_interrupt_resolution =
        detail::interrupt_runtime::resolve_external_resolution_kind(*policy);
  }
  if (!((external_interrupt && policy->auto_persist_external_interrupt) ||
        (!external_interrupt && policy->manual_persist_internal_interrupt))) {
    return detail::ready_graph_unit_sender();
  }
  return detail::bridge_graph_sender(
      std::move(capture_sender) |
      stdexec::then([persist = std::forward<persist_fn_t>(persist)](
                        wh::core::result<graph_value> captured) mutable
                        -> wh::core::result<graph_value> {
        if (captured.has_error()) {
          return wh::core::result<graph_value>::failure(captured.error());
        }
        persist();
        return detail::make_graph_unit_value();
      }));
}

inline auto detail::invoke_runtime::dag_runtime::enqueue_dependents(
    const std::uint32_t source_node_id) -> void {
  const auto &index = session_.compiled_graph_index();
  for (const auto edge_id : index.outgoing_control(source_node_id)) {
    const auto target = index.indexed_edges[edge_id].to;
    const auto enqueued =
        session_.graph_options().dispatch_policy == graph_dispatch_policy::same_wave
            ? frontier().enqueue_current(target)
            : frontier().enqueue_next(target);
    if (enqueued) {
      session_.emit_debug(graph_debug_stream_event::decision_kind::enqueue,
                          target, session_.invoke_state().step_count);
    }
  }
}

inline auto detail::invoke_runtime::dag_runtime::promote_next_wave() -> bool {
  return frontier().promote_next_wave();
}

inline auto
detail::invoke_runtime::invoke_session::check_external_interrupt_boundary()
    -> wh::core::result<bool> {
  auto &interrupt = interrupt_state();
  auto &invoke = invoke_state();
  auto boundary_state =
      detail::interrupt_runtime::external_interrupt_boundary_state{
          .wait_mode_active = interrupt.wait_mode_active,
          .deadline = interrupt.deadline,
      };
  auto handled = detail::interrupt_runtime::handle_external_boundary(
      context_, invoke.outputs, interrupt.policy_latch, interrupt.policy,
      boundary_state, [this](const bool external_interrupt) {
        request_freeze(external_interrupt);
        return wh::core::result<void>{};
      });
  interrupt.wait_mode_active = boundary_state.wait_mode_active;
  interrupt.deadline = boundary_state.deadline;
  return handled;
}

} // namespace wh::compose
