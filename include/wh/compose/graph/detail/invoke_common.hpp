// Defines out-of-line invoke runtime helpers shared across DAG and Pregel.
#pragma once

#include <charconv>

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

inline auto detail::invoke_runtime::run_state::initialize_runtime_node_caches()
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
detail::invoke_runtime::run_state::initialize_resolved_component_options()
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
detail::invoke_runtime::run_state::initialize_resolved_node_observations()
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
detail::invoke_runtime::run_state::initialize_resolved_state_handlers()
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

inline auto detail::invoke_runtime::run_state::initialize_trace_state()
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
detail::invoke_runtime::run_state::next_node_trace(const std::uint32_t node_id)
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

inline auto detail::invoke_runtime::run_state::runtime_node_path(
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

inline auto detail::invoke_runtime::run_state::runtime_stream_scope(
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

inline auto detail::invoke_runtime::run_state::runtime_node_execution_address(
    const std::uint32_t node_id) -> const wh::core::address & {
  auto &cache = cache_state();
  wh_precondition(node_id < cache.runtime_node_execution_addresses.size());
  auto &location = cache.runtime_node_execution_addresses[node_id];
  if (location.empty()) {
    location = owner_->make_node_execution_address(runtime_node_path(node_id));
  }
  return location;
}

inline auto detail::invoke_runtime::run_state::transition_log() noexcept
    -> graph_transition_log & {
  return invoke_state().outputs.transition_log;
}

inline auto detail::invoke_runtime::run_state::initialize(
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
  rerun_state_.reset(total_nodes);

  detail::process_runtime::bind_parent_process_state(
      process_state_, invoke.parent_process_state);
  transition_log().clear();

  invoke.run_id = owner_->next_invoke_run_id();
  const auto restore_scope =
      invoke.parent_process_state == nullptr
          ? detail::checkpoint_runtime::restore_scope::full
          : detail::checkpoint_runtime::restore_scope::forwarded_only;
  auto restored = owner_->maybe_restore_from_checkpoint(
      input, context_, state_table_, rerun_state_, invoke.config,
      skip_state_pre_handlers_, restore_scope, invoke.path_prefix,
      invoke.outputs, *invoke.forwarded_checkpoints);
  if (restored.has_error()) {
    persist_checkpoint_best_effort();
    init_error_ = restored.error();
    return;
  }
  auto runtime_resume =
      owner_->apply_runtime_resume_controls(context_, invoke.config);
  if (runtime_resume.has_error()) {
    persist_checkpoint_best_effort();
    init_error_ = runtime_resume.error();
    return;
  }
  auto resume_state_overrides =
      detail::interrupt_runtime::apply_resume_data_state_overrides(
          context_, state_table_);
  if (resume_state_overrides.has_error()) {
    persist_checkpoint_best_effort();
    init_error_ = resume_state_overrides.error();
    return;
  }

  auto step_budget = owner_->resolve_step_budget(invoke.config, call_scope);
  if (step_budget.has_error()) {
    owner_->publish_graph_run_error(
        invoke.outputs, std::nullopt, {}, compose_error_phase::schedule,
        step_budget.error(), "step budget resolution failed");
    persist_checkpoint_best_effort();
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
  progress_state_.reset(index.nodes_by_id.size());
  node_local_process_states_.clear();
  node_local_process_states_.resize(index.nodes_by_id.size());
  initialize_runtime_node_caches();
  invoke.outputs.last_completed_nodes.reserve(total_nodes);
  if (cache.collect_transition_log) {
    transition_log().reserve(total_nodes * 4U);
  }
  interrupt.policy = resolve_external_interrupt_policy(invoke.bound_call_scope);
  interrupt.policy_latch = {};
  const auto start_node_id = index.start_id;
  const auto *start_node = start_node_id < index.nodes_by_id.size()
                               ? index.nodes_by_id[start_node_id]
                               : nullptr;
  if (start_node == nullptr) {
    init_error_ = wh::core::errc::not_found;
    return;
  }
  const auto start_contract = start_node->meta.output_contract;

  if (invoke.retain_inputs) {
    auto start_rerun = std::move(input);
    auto start_output = start_contract == node_contract::stream
                            ? detail::fork_graph_reader_payload(start_rerun)
                            : fork_graph_value(start_rerun);
    if (start_output.has_error()) {
      init_error_ = start_output.error();
      return;
    }
    auto stored_start = owner_->store_node_output(
        start_node_id, io_storage_, std::move(start_output).value());
    if (stored_start.has_error()) {
      init_error_ = stored_start.error();
      return;
    }
    rerun_state_.store(start_node_id, std::move(start_rerun));
  } else {
    auto stored_start =
        owner_->store_node_output(start_node_id, io_storage_, std::move(input));
    if (stored_start.has_error()) {
      init_error_ = stored_start.error();
      return;
    }
  }
  node_states()[start_node_id] = node_state::executed;

  auto start_output_view = owner_->view_node_output(start_node_id, io_storage_);
  if (start_output_view.has_error()) {
    init_error_ = start_output_view.error();
    return;
  }
  auto start_branch =
      start_contract == node_contract::value
          ? owner_->evaluate_value_branch_indexed(
                start_node_id, start_output_view.value(), context_,
                invoke.bound_call_scope)
          : owner_->evaluate_stream_branch_indexed(
                start_node_id, io_storage_, context_, invoke.bound_call_scope);
  if (start_branch.has_error()) {
    state_table_.update(start_node_id, graph_node_lifecycle_state::failed, 1U,
                        start_branch.error());
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
    persist_checkpoint_best_effort();
    init_error_ = start_branch.error();
    return;
  }
  auto start_selection = start_branch.value();
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
  invoke.start_entry_selection = std::move(start_selection);
}

inline auto
detail::invoke_runtime::run_state::immediate_success(graph_value value)
    -> graph_sender {
  publish_runtime_outputs();
  return detail::ready_graph_sender(
      wh::core::result<graph_value>{std::move(value)});
}

inline auto detail::invoke_runtime::run_state::immediate_failure(
    const wh::core::error_code code) -> graph_sender {
  publish_runtime_outputs();
  return detail::failure_graph_sender(code);
}

inline auto detail::invoke_runtime::run_state::persist_checkpoint_best_effort()
    -> void {
  auto &invoke = invoke_state();
  [[maybe_unused]] const auto persisted = owner_->maybe_persist_checkpoint(
      context_, state_table_, rerun_state_, invoke.config, invoke.outputs);
}

inline auto detail::invoke_runtime::run_state::publish_runtime_outputs()
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

inline auto detail::invoke_runtime::run_state::emit_debug(
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

inline auto detail::invoke_runtime::run_state::append_transition(
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

inline auto detail::invoke_runtime::run_state::append_transition(
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

inline auto detail::invoke_runtime::run_state::append_transition(
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

inline auto detail::invoke_runtime::run_state::append_transition(
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

inline auto detail::invoke_runtime::run_state::evaluate_resume_match(
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

inline auto detail::invoke_runtime::run_state::control_slot_id() const noexcept
    -> std::uint32_t {
  return static_cast<std::uint32_t>(node_count());
}

inline auto detail::invoke_runtime::run_state::request_freeze(
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

inline auto detail::invoke_runtime::run_state::freeze_requested() const noexcept
    -> bool {
  return interrupt_state().freeze_requested;
}

inline auto detail::invoke_runtime::run_state::freeze_external() const noexcept
    -> bool {
  return interrupt_state().freeze_external;
}

inline auto detail::invoke_runtime::run_state::make_freeze_sender(
    graph_sender capture_sender, const bool external_interrupt)
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
      stdexec::then([this](wh::core::result<graph_value> captured)
                        -> wh::core::result<graph_value> {
        if (captured.has_error()) {
          return wh::core::result<graph_value>::failure(captured.error());
        }
        persist_checkpoint_best_effort();
        return detail::make_graph_unit_value();
      }));
}

inline auto detail::invoke_runtime::dag_run_state::enqueue_dependents(
    const std::uint32_t source_node_id) -> void {
  const auto &index = compiled_graph_index();
  for (const auto edge_id : index.outgoing_control(source_node_id)) {
    const auto target = index.indexed_edges[edge_id].to;
    const auto enqueued =
        graph_options().dispatch_policy == graph_dispatch_policy::same_wave
            ? frontier().enqueue_current(target)
            : frontier().enqueue_next(target);
    if (enqueued) {
      emit_debug(graph_debug_stream_event::decision_kind::enqueue, target,
                 invoke_state().step_count);
    }
  }
}

inline auto detail::invoke_runtime::dag_run_state::promote_next_wave() -> bool {
  return frontier().promote_next_wave();
}

inline auto
detail::invoke_runtime::run_state::check_external_interrupt_boundary()
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

inline auto
detail::invoke_runtime::run_state::make_input_frame(const std::uint32_t node_id,
                                                    const std::size_t step)
    -> wh::core::result<node_frame> {
  const auto &index = compiled_graph_index();
  const auto *node = index.nodes_by_id[node_id];
  if (node == nullptr) {
    return wh::core::result<node_frame>::failure(wh::core::errc::not_found);
  }

  node_states()[node_id] = node_state::running;
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

inline auto detail::invoke_runtime::run_state::begin_state_pre(
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
    persist_checkpoint_best_effort();
    return wh::core::result<state_step>::failure(resume_interrupt.error());
  }
  if (resume_interrupt.value().has_value()) {
    context_.interrupt_info = wh::compose::to_interrupt_context(
        std::move(resume_interrupt.value().value()));
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
               frame.node_id, frame.cause.step);
    rerun_state_.store(frame.node_id, std::move(input));
    node_states()[frame.node_id] = node_state::pending;
    request_freeze(false);
    return wh::core::result<state_step>::failure(wh::core::errc::canceled);
  }

  auto pre_interrupt = owner_->evaluate_interrupt_hook(
      context_, invoke.config.interrupt_pre_hook, current_node_key, input);
  if (pre_interrupt.has_error()) {
    persist_checkpoint_best_effort();
    return wh::core::result<state_step>::failure(pre_interrupt.error());
  }
  if (pre_interrupt.value().has_value()) {
    context_.interrupt_info = wh::compose::to_interrupt_context(
        std::move(pre_interrupt.value().value()));
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
               frame.node_id, frame.cause.step);
    rerun_state_.store(frame.node_id, std::move(input));
    node_states()[frame.node_id] = node_state::pending;
    request_freeze(false);
    return wh::core::result<state_step>::failure(wh::core::errc::canceled);
  }

  auto node_local_state =
      detail::process_runtime::acquire_node_local_process_state(
          node_local_process_states_, frame.node_id, process_state_);
  if (node_local_state.has_error()) {
    persist_checkpoint_best_effort();
    return wh::core::result<state_step>::failure(node_local_state.error());
  }
  auto node_local_scope = std::move(node_local_state).value();
  struct node_local_scope_guard {
    run_state *state{nullptr};
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
    persist_checkpoint_best_effort();
    return wh::core::result<state_step>::failure(node_local_ref.error());
  }

  frame.node_scope.path = runtime_node_path(frame.node_id);
  frame.node_scope.local_process_state =
      std::addressof(node_local_ref.value().get());

  const auto should_skip_pre_handler =
      skip_state_pre_handlers_ && rerun_state_.restored(frame.node_id);
  if (!should_skip_pre_handler) {
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
      persist_checkpoint_best_effort();
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

inline auto detail::invoke_runtime::run_state::prepare_execution_input(
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

inline auto detail::invoke_runtime::run_state::should_retain_input(
    const node_frame &frame) const noexcept -> bool {
  return invoke_state().retain_inputs || frame.retry_budget > 0U;
}

inline auto detail::invoke_runtime::run_state::finalize_node_frame(
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
    persist_checkpoint_best_effort();
    return wh::core::result<node_frame>::failure(
        wh::core::errc::contract_violation);
  }

  frame.stage = invoke_stage::node;
  frame.retry_budget = node_retry_budget;
  frame.timeout_budget = node_timeout_budget;
  if (should_retain_input(frame)) {
    rerun_state_.store(frame.node_id, std::move(input));
  } else {
    frame.node_input.emplace(std::move(input));
  }
  detail::node_runtime_access::reset(frame.node_runtime,
                                     effective_parallel_gate);
  return frame;
}

inline auto detail::invoke_runtime::run_state::begin_state_post(
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
    persist_checkpoint_best_effort();
    frame.node_local_scope.release(node_local_process_states_);
    return wh::core::result<state_step>::failure(post_state.error());
  }
  return state_step{
      .frame = std::move(frame),
      .payload = std::move(output),
      .sender = std::nullopt,
  };
}

inline auto detail::invoke_runtime::run_state::fail_node_stage(
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
  persist_checkpoint_best_effort();
  frame.node_local_scope.release(node_local_process_states_);
  return wh::core::result<void>::failure(code);
}

inline auto detail::invoke_runtime::run_state::bind_node_runtime_call_options(
    node_frame &frame, const graph_call_scope &bound_call_options,
    run_state *state) noexcept -> void {
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

inline auto detail::invoke_runtime::run_state::start_nested_from_runtime(
    const void *state_ptr, const graph &nested_graph,
    wh::core::run_context &context, graph_value &input,
    const graph_call_scope *call_options, const node_path *path_prefix,
    graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const graph_node_trace *parent_trace) -> graph_sender {
  const auto *state = static_cast<const run_state *>(state_ptr);
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
detail::invoke_runtime::run_state::nested_graph_entry() const noexcept
    -> wh::compose::nested_graph_entry {
  return wh::compose::nested_graph_entry{
      .state = this,
      .start = &start_nested_from_runtime,
  };
}

inline auto detail::invoke_runtime::run_state::timeout_scheduler() noexcept
    -> exec::timed_thread_scheduler {
  static exec::timed_thread_context context{};
  return context.get_scheduler();
}

inline auto detail::invoke_runtime::run_state::make_node_timeout_failure(
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

inline auto detail::invoke_runtime::run_state::apply_node_timeout(
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
inline auto detail::invoke_runtime::run_state::make_async_timed_node_sender(
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
inline auto detail::invoke_runtime::run_state::run_sync_node_execution(
    const compiled_node &node, graph_value &input_value,
    wh::core::run_context &context, const graph_call_scope &bound_call_options,
    run_state *state, node_frame &frame, retry_fn_t retry_fn)
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

inline auto detail::invoke_runtime::run_state::make_async_node_attempt_sender(
    const compiled_node &node, graph_value &input_value,
    wh::core::run_context &context, const graph_call_scope &bound_call_options,
    run_state *state, node_frame &frame) -> graph_sender {
  bind_node_runtime_call_options(frame, bound_call_options, state);
  const auto attempt_start = std::chrono::steady_clock::now();
  return make_async_timed_node_sender(
      run_compiled_async_node(node, input_value, context, frame.node_runtime),
      state->invoke_state().outputs, frame, attempt_start);
}

inline auto detail::invoke_runtime::run_state::finish_graph_status()
    -> wh::core::result<graph_value> {
  auto &invoke = invoke_state();
  const auto final_node_id = end_id();
  const auto &final_node_key = node_key(final_node_id);
  if (node_states()[final_node_id] != node_state::executed) {
    owner_->publish_graph_run_error(
        invoke.outputs, runtime_node_path(final_node_id), final_node_key,
        compose_error_phase::execute, wh::core::errc::contract_violation,
        "end node was not executed");
    owner_->publish_last_completed_nodes(invoke.outputs, node_states());
    persist_checkpoint_best_effort();
    return wh::core::result<graph_value>::failure(
        wh::core::errc::contract_violation);
  }
  if (!output_valid().test(final_node_id)) {
    owner_->publish_graph_run_error(
        invoke.outputs, runtime_node_path(final_node_id), final_node_key,
        compose_error_phase::execute, wh::core::errc::not_found,
        "end node output not found");
    persist_checkpoint_best_effort();
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  auto final_output = owner_->take_node_output(final_node_id, io_storage_);
  if (final_output.has_value()) {
    output_valid().clear(final_node_id);
  }
  persist_checkpoint_best_effort();
  return final_output;
}

inline auto detail::invoke_runtime::run_state::finish_graph() -> graph_sender {
  auto status = finish_graph_status();
  if (status.has_error()) {
    return immediate_failure(status.error());
  }
  return immediate_success(std::move(status).value());
}

} // namespace wh::compose
