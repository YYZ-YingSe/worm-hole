// Defines out-of-line invoke runtime helpers shared across DAG and Pregel.
#pragma once

#include <charconv>

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

namespace detail {

[[nodiscard]] inline auto format_trace_token(const std::string_view prefix,
                                             const std::uint64_t run_id,
                                             const std::optional<std::uint64_t> sequence =
                                                 std::nullopt)
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
  const auto node_count = owner_->compiled_execution_index_.index.nodes_by_id.size();
  runtime_node_paths_.clear();
  runtime_node_paths_.resize(node_count);
  runtime_stream_scopes_.clear();
  if (collect_transition_log_ || emit_state_snapshot_events_ ||
      emit_state_delta_events_ || emit_runtime_message_events_ ||
      emit_custom_events_) {
    runtime_stream_scopes_.resize(node_count);
  }
  runtime_node_execution_addresses_.clear();
  runtime_node_execution_addresses_.resize(node_count);
}

inline auto detail::invoke_runtime::run_state::initialize_resolved_component_options()
    -> void {
  const auto node_count = owner_->compiled_execution_index_.index.nodes_by_id.size();
  resolved_component_options_.clear();
  resolved_component_options_.resize(node_count);
  if (!has_component_option_overrides_) {
    return;
  }

  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    resolved_component_options_[node_id] = resolve_graph_component_option_map(
        bound_call_scope_, owner_->make_node_designation_path(node_id));
  }
}

inline auto detail::invoke_runtime::run_state::initialize_resolved_node_observations()
    -> void {
  const auto node_count = owner_->compiled_execution_index_.index.nodes_by_id.size();
  resolved_node_observations_.clear();
  resolved_node_observations_.resize(node_count);

  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    const auto *node = owner_->compiled_execution_index_.index.nodes_by_id[node_id];
    if (node == nullptr) {
      continue;
    }

    auto &resolved = resolved_node_observations_[node_id];
    const auto &defaults = node->meta.options.observation;
    resolved.callbacks_enabled = defaults.callbacks_enabled;
    resolved.local_callbacks = defaults.local_callbacks;
    if (!defaults.allow_invoke_override) {
      continue;
    }

    const auto path = owner_->make_node_designation_path(node_id);
    for (const auto &rule : bound_call_scope_.options().node_observations) {
      if (!matches_node_observation(bound_call_scope_, path, rule)) {
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

inline auto detail::invoke_runtime::run_state::initialize_resolved_state_handlers()
    -> wh::core::result<void> {
  const auto node_count = owner_->compiled_execution_index_.index.nodes_by_id.size();
  resolved_state_handlers_.clear();
  resolved_state_handlers_.resize(node_count, nullptr);

  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    const auto *node = owner_->compiled_execution_index_.index.nodes_by_id[node_id];
    if (node == nullptr) {
      continue;
    }
    auto handlers = detail::state_runtime::resolve_node_state_handlers(
        invoke_config_.state_handlers, owner_->compiled_execution_index_.index.id_to_key[node_id],
        node->meta.options);
    if (handlers.has_error()) {
      return wh::core::result<void>::failure(handlers.error());
    }
    resolved_state_handlers_[node_id] = handlers.value();
  }
  return {};
}

inline auto detail::invoke_runtime::run_state::initialize_trace_state() -> void {
  trace_state_ = detail::runtime_state::graph_trace_state{};
  if (bound_call_scope_.trace().has_value()) {
    trace_state_.trace_id = bound_call_scope_.trace()->trace_id;
    trace_state_.parent_span_id = bound_call_scope_.trace()->parent_span_id;
  }
  if (trace_state_.trace_id.empty()) {
    trace_state_.trace_id = detail::format_trace_token("g", run_id_);
  }
  trace_state_.graph_span_id = detail::format_trace_token("gs", run_id_, 0U);
  trace_state_.next_span_sequence = 1U;
}

inline auto detail::invoke_runtime::run_state::next_node_trace(
    const std::uint32_t node_id) -> graph_node_trace {
  graph_node_trace trace{};
  trace.trace_id = trace_state_.trace_id;
  trace.span_id = detail::format_trace_token(
      "ns", run_id_, trace_state_.next_span_sequence++);
  trace.parent_span_id = trace_state_.graph_span_id;
  trace.path = std::addressof(runtime_node_execution_address(node_id));
  return trace;
}

inline auto detail::invoke_runtime::run_state::runtime_node_path(
    const std::uint32_t node_id) -> const node_path & {
  auto &path = runtime_node_paths_[node_id];
  if (path.empty()) {
    path = owner_->make_runtime_node_path(path_prefix_, node_id);
  }
  return path;
}

inline auto detail::invoke_runtime::run_state::runtime_stream_scope(
    const std::uint32_t node_id) -> const graph_event_scope & {
  const auto node_count = owner_->compiled_execution_index_.index.nodes_by_id.size();
  if (runtime_stream_scopes_.size() != node_count) {
    runtime_stream_scopes_.resize(node_count);
  }
  auto &scope = runtime_stream_scopes_[node_id];
  if (scope.node.empty()) {
    scope = make_graph_event_scope(
        owner_->options_.name, owner_->compiled_execution_index_.index.id_to_key[node_id],
        runtime_node_path(node_id));
  }
  return scope;
}

inline auto detail::invoke_runtime::run_state::runtime_node_execution_address(
    const std::uint32_t node_id) -> const wh::core::address & {
  auto &location = runtime_node_execution_addresses_[node_id];
  if (location.empty()) {
    location = owner_->make_node_execution_address(runtime_node_path(node_id));
  }
  return location;
}

inline auto detail::invoke_runtime::run_state::transition_log() noexcept
    -> graph_transition_log & {
  return invoke_outputs_.transition_log;
}

inline auto detail::invoke_runtime::run_state::initialize(
    graph_value &&input, graph_call_scope call_scope) -> void {
  if (owner_->compiled_execution_index_.index.nodes_by_id.empty()) {
    owner_->publish_graph_run_error(
        invoke_outputs_, std::nullopt, {}, compose_error_phase::execute,
        wh::core::errc::contract_violation,
        "compiled runtime index is empty");
    init_error_ = wh::core::errc::contract_violation;
    return;
  }
  if (parent_state_ != nullptr) {
    services_ = parent_state_->services_;
    invoke_config_ = parent_state_->invoke_config_;
    forwarded_checkpoints_ = parent_state_->forwarded_checkpoints_;
  } else {
    invoke_config_.state_handlers =
        services_ != nullptr ? services_->state_handlers : nullptr;
    invoke_config_.values_merge_registry =
        services_ != nullptr ? services_->values_merge_registry : nullptr;
    invoke_config_.stream_concat_registry =
        services_ != nullptr ? services_->stream_concat_registry : nullptr;
    invoke_config_.checkpoint_store =
        services_ != nullptr ? services_->checkpoint.store : nullptr;
    invoke_config_.checkpoint_backend =
        services_ != nullptr ? services_->checkpoint.backend : nullptr;
    invoke_config_.checkpoint_stream_codecs =
        services_ != nullptr ? services_->checkpoint.stream_codecs : nullptr;
    invoke_config_.checkpoint_serializer =
        services_ != nullptr ? services_->checkpoint.serializer : nullptr;
    invoke_config_.checkpoint_load = invoke_controls_.checkpoint.load;
    invoke_config_.checkpoint_save = invoke_controls_.checkpoint.save;
    invoke_config_.checkpoint_before_load =
        invoke_controls_.checkpoint.before_load;
    invoke_config_.checkpoint_before_load_nodes =
        invoke_controls_.checkpoint.before_load_nodes;
    invoke_config_.checkpoint_after_load =
        invoke_controls_.checkpoint.after_load;
    invoke_config_.checkpoint_after_load_nodes =
        invoke_controls_.checkpoint.after_load_nodes;
    invoke_config_.checkpoint_before_save =
        invoke_controls_.checkpoint.before_save;
    invoke_config_.checkpoint_before_save_nodes =
        invoke_controls_.checkpoint.before_save_nodes;
    invoke_config_.checkpoint_after_save =
        invoke_controls_.checkpoint.after_save;
    invoke_config_.checkpoint_after_save_nodes =
        invoke_controls_.checkpoint.after_save_nodes;
    invoke_config_.interrupt_pre_hook = invoke_controls_.interrupt.pre_hook;
    invoke_config_.interrupt_post_hook = invoke_controls_.interrupt.post_hook;
    invoke_config_.resume_contexts = invoke_controls_.resume.contexts;
    invoke_config_.subgraph_interrupt_signals =
        invoke_controls_.interrupt.subgraph_signals;
    invoke_config_.resume_decision = invoke_controls_.resume.decision;
    invoke_config_.batch_resume_items = invoke_controls_.resume.batch_items;
    invoke_config_.reinterrupt_unmatched =
        invoke_controls_.resume.reinterrupt_unmatched;
    invoke_config_.pregel_max_steps_override =
        invoke_controls_.schedule.pregel_max_steps;
    if (invoke_controls_.schedule.branch_merge.has_value()) {
      invoke_config_.branch_merge = *invoke_controls_.schedule.branch_merge;
    }
    owned_forwarded_checkpoints_ =
        std::move(invoke_controls_.checkpoint.forwarded_once);
    forwarded_checkpoints_ = std::addressof(owned_forwarded_checkpoints_);
  }

  auto checkpoint_config = detail::checkpoint_runtime::validate_runtime_configuration(
      invoke_config_, invoke_outputs_);
  if (checkpoint_config.has_error()) {
    owner_->publish_graph_run_error(
        invoke_outputs_, std::nullopt, {}, compose_error_phase::checkpoint,
        checkpoint_config.error(), "checkpoint runtime configuration invalid");
    init_error_ = checkpoint_config.error();
    return;
  }
  auto call_options_validated =
      owner_->validate_call_scope_for_runtime(call_scope);
  if (call_options_validated.has_error()) {
    owner_->publish_graph_run_error(
        invoke_outputs_, std::nullopt, {}, compose_error_phase::schedule,
        call_options_validated.error(), "call options validation failed");
    init_error_ = call_options_validated.error();
    return;
  }
  const bool has_checkpoint_backend =
      invoke_config_.checkpoint_store != nullptr ||
      invoke_config_.checkpoint_backend != nullptr;
  retain_inputs_ =
      has_checkpoint_backend || context_.resume_info.has_value() ||
      invoke_config_.interrupt_pre_hook || invoke_config_.interrupt_post_hook ||
      invoke_config_.resume_decision.has_value() ||
      !invoke_config_.batch_resume_items.empty() ||
      !invoke_config_.resume_contexts.empty() ||
      !invoke_config_.subgraph_interrupt_signals.empty();

  const auto node_count = owner_->compiled_execution_index_.index.nodes_by_id.size();
  state_table_.reset(owner_->compiled_execution_index_.index.id_to_key);
  rerun_state_.reset(node_count);

  detail::process_runtime::bind_parent_process_state(process_state_,
                                                     parent_process_state_);
  transition_log().clear();

  run_id_ = owner_->next_invoke_run_id();
  const auto restore_scope =
      parent_process_state_ == nullptr
          ? detail::checkpoint_runtime::restore_scope::full
          : detail::checkpoint_runtime::restore_scope::forwarded_only;
  auto restored = owner_->maybe_restore_from_checkpoint(
      input, context_, state_table_, rerun_state_, invoke_config_,
      skip_state_pre_handlers_,
      restore_scope, path_prefix_, invoke_outputs_, *forwarded_checkpoints_);
  if (restored.has_error()) {
    persist_checkpoint_best_effort();
    init_error_ = restored.error();
    return;
  }
  auto runtime_resume = owner_->apply_runtime_resume_controls(context_, invoke_config_);
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

  auto step_budget = owner_->resolve_step_budget(invoke_config_, call_scope);
  if (step_budget.has_error()) {
    owner_->publish_graph_run_error(
        invoke_outputs_, std::nullopt, {}, compose_error_phase::schedule,
        step_budget.error(), "step budget resolution failed");
    persist_checkpoint_best_effort();
    init_error_ = step_budget.error();
    return;
  }
  step_budget_ = step_budget.value();

  bound_call_scope_ = std::move(call_scope);
  has_component_option_overrides_ =
      !bound_call_scope_.component_defaults().empty() ||
      !bound_call_scope_.options().component_overrides.empty();
  initialize_resolved_component_options();
  initialize_resolved_node_observations();
  auto resolved_state_handlers = initialize_resolved_state_handlers();
  if (resolved_state_handlers.has_error()) {
    owner_->publish_graph_run_error(
        invoke_outputs_, std::nullopt, {}, compose_error_phase::execute,
        resolved_state_handlers.error(), "state handler resolution failed");
    init_error_ = resolved_state_handlers.error();
    return;
  }
  initialize_trace_state();
  emit_debug_events_ = should_emit_graph_debug_event(bound_call_scope_);
  collect_transition_log_ = bound_call_scope_.record_transition_log();
  invoke_outputs_.publish_transition_log = collect_transition_log_;
  emit_state_snapshot_events_ =
      has_graph_stream_subscription(bound_call_scope_,
                                    graph_stream_channel_kind::state_snapshot);
  emit_state_delta_events_ =
      has_graph_stream_subscription(bound_call_scope_,
                                    graph_stream_channel_kind::state_delta);
  emit_runtime_message_events_ =
      has_graph_stream_subscription(bound_call_scope_,
                                    graph_stream_channel_kind::message);
  emit_custom_events_ = std::ranges::any_of(
      bound_call_scope_.options().stream_subscriptions,
      [](const graph_stream_subscription &subscription) {
        return subscription.enabled &&
               subscription.kind == graph_stream_channel_kind::custom &&
               !subscription.custom_channel.empty();
      });
  scratch_.reset(owner_->compiled_execution_index_.index.nodes_by_id.size(),
                 owner_->compiled_execution_index_.index.indexed_edges.size());
  node_local_process_states_.clear();
  node_local_process_states_.resize(owner_->compiled_execution_index_.index.nodes_by_id.size());
  initialize_runtime_node_caches();
  invoke_outputs_.last_completed_nodes.reserve(node_count);
  deferred_ready_queue_.clear();
  deferred_ready_queue_.reserve(node_count);
  if (collect_transition_log_) {
    transition_log().reserve(node_count * 4U);
  }
  external_interrupt_policy_ = resolve_external_interrupt_policy(bound_call_scope_);
  external_interrupt_policy_latch_ = {};

  if (retain_inputs_) {
    auto start_rerun = std::move(input);
    auto start_output = fork_graph_value(start_rerun);
    if (start_output.has_error()) {
      init_error_ = start_output.error();
      return;
    }
    auto stored_start =
        owner_->store_node_output(owner_->compiled_execution_index_.index.start_id, scratch_,
                                  std::move(start_output).value());
    if (stored_start.has_error()) {
      init_error_ = stored_start.error();
      return;
    }
    rerun_state_.store(owner_->compiled_execution_index_.index.start_id,
                       std::move(start_rerun));
  } else {
    auto stored_start =
        owner_->store_node_output(owner_->compiled_execution_index_.index.start_id, scratch_,
                                  std::move(input));
    if (stored_start.has_error()) {
      init_error_ = stored_start.error();
      return;
    }
  }
  node_states()[owner_->compiled_execution_index_.index.start_id] = node_state::executed;

  auto start_output_view =
      owner_->view_node_output(owner_->compiled_execution_index_.index.start_id, scratch_);
  if (start_output_view.has_error()) {
    init_error_ = start_output_view.error();
    return;
  }
  auto start_branch = owner_->evaluate_branch_indexed(
      owner_->compiled_execution_index_.index.start_id, start_output_view.value(), context_,
      bound_call_scope_);
  if (start_branch.has_error()) {
    state_table_.update(owner_->compiled_execution_index_.index.start_id,
                        graph_node_lifecycle_state::failed, 1U,
                        start_branch.error());
    append_transition(owner_->compiled_execution_index_.index.start_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause =
                              graph_state_cause{
                                  .run_id = run_id_,
                                  .step = 0U,
                                  .node_key = std::string{graph_start_node_key},
                              },
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    persist_checkpoint_best_effort();
    init_error_ = start_branch.error();
    return;
  }
  auto start_branch_committed = owner_->commit_branch_selection(
      owner_->compiled_execution_index_.index.start_id, std::move(start_branch).value(), scratch_,
      invoke_config_);
  if (start_branch_committed.has_error()) {
    state_table_.update(owner_->compiled_execution_index_.index.start_id,
                        graph_node_lifecycle_state::failed, 1U,
                        start_branch_committed.error());
    append_transition(owner_->compiled_execution_index_.index.start_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause =
                              graph_state_cause{
                                  .run_id = run_id_,
                                  .step = 0U,
                                  .node_key = std::string{graph_start_node_key},
                              },
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    persist_checkpoint_best_effort();
    init_error_ = start_branch_committed.error();
    return;
  }
  state_table_.update(owner_->compiled_execution_index_.index.start_id,
                      graph_node_lifecycle_state::completed, 0U, std::nullopt);
  append_transition(owner_->compiled_execution_index_.index.start_id,
                    graph_state_transition_event{
                        .kind = graph_state_transition_kind::route_commit,
                        .cause =
                            graph_state_cause{
                                .run_id = run_id_,
                                .step = 0U,
                                .node_key = std::string{graph_start_node_key},
                            },
                        .lifecycle = graph_node_lifecycle_state::completed,
                    });

  auto start_streams = owner_->refresh_source_readers(
      owner_->compiled_execution_index_.index.start_id, scratch_, node_states(),
      branch_states(), context_);
  if (start_streams.has_error()) {
    init_error_ = start_streams.error();
    return;
  }

  enqueue_dependents(owner_->compiled_execution_index_.index.start_id);
  for (const auto node_id : owner_->compiled_execution_index_.index.allow_no_control_ids) {
    if (queued().set_if_unset(node_id)) {
      ready_queue().push_back(node_id);
      emit_debug(graph_debug_stream_event::decision_kind::enqueue, node_id,
                 step_count_);
    }
  }

  if (!owner_->options_.eager) {
    flush_deferred_enqueue();
  }
  current_batch_end_ = ready_queue().size();
}

inline auto detail::invoke_runtime::run_state::immediate_success(graph_value value)
    -> graph_sender {
  publish_runtime_outputs();
  return detail::ready_graph_sender(wh::core::result<graph_value>{std::move(value)});
}

inline auto detail::invoke_runtime::run_state::immediate_failure(
    const wh::core::error_code code) -> graph_sender {
  publish_runtime_outputs();
  return detail::failure_graph_sender(code);
}

inline auto detail::invoke_runtime::run_state::persist_checkpoint_best_effort()
    -> void {
  [[maybe_unused]] const auto persisted =
      owner_->maybe_persist_checkpoint(context_, state_table_, rerun_state_,
                                       invoke_config_, invoke_outputs_);
}

inline auto detail::invoke_runtime::run_state::publish_runtime_outputs() -> void {
  if (forwarded_checkpoints_ != nullptr &&
      invoke_outputs_.remaining_forwarded_checkpoint_keys.empty()) {
    invoke_outputs_.remaining_forwarded_checkpoint_keys.reserve(
        forwarded_checkpoints_->size());
    for (const auto &entry : *forwarded_checkpoints_) {
      invoke_outputs_.remaining_forwarded_checkpoint_keys.push_back(entry.first);
    }
    std::sort(invoke_outputs_.remaining_forwarded_checkpoint_keys.begin(),
              invoke_outputs_.remaining_forwarded_checkpoint_keys.end());
  }
  if (nested_outputs_ != nullptr) {
    detail::runtime_state::merge_nested_outputs(*nested_outputs_,
                                                std::move(invoke_outputs_));
    return;
  }
  if (published_outputs_ != nullptr) {
    published_outputs_->remaining_forwarded_checkpoint_keys =
        std::move(invoke_outputs_.remaining_forwarded_checkpoint_keys);
    detail::runtime_state::merge_nested_outputs(*published_outputs_,
                                                std::move(invoke_outputs_));
  }
}

inline auto detail::invoke_runtime::run_state::emit_debug(
    const graph_debug_stream_event::decision_kind decision,
    const std::uint32_t node_id, const std::size_t step) -> void {
  if (!emit_debug_events_) {
    return;
  }
  owner_->emit_debug_stream_event(context_, invoke_outputs_, bound_call_scope_,
                                  decision, node_id, runtime_node_path(node_id),
                                  step);
}

inline auto detail::invoke_runtime::run_state::append_transition(
    const graph_state_transition_event &event) -> void {
  if (!collect_transition_log_ && !emit_state_snapshot_events_ &&
      !emit_state_delta_events_ && !emit_runtime_message_events_ &&
      !emit_custom_events_) {
    return;
  }
  const auto node_id_iter = owner_->compiled_execution_index_.index.key_to_id.find(event.cause.node_key);
  if (node_id_iter == owner_->compiled_execution_index_.index.key_to_id.end()) {
    detail::stream_runtime::append_state_transition(
        transition_log(), invoke_outputs_, bound_call_scope_, event,
        owner_->make_stream_scope(event.cause.node_key), collect_transition_log_,
        emit_state_snapshot_events_, emit_state_delta_events_,
        emit_runtime_message_events_, emit_custom_events_);
    return;
  }
  detail::stream_runtime::append_state_transition(
      transition_log(), invoke_outputs_, bound_call_scope_, event,
      runtime_stream_scope(node_id_iter->second), collect_transition_log_,
      emit_state_snapshot_events_, emit_state_delta_events_,
      emit_runtime_message_events_, emit_custom_events_);
}

inline auto detail::invoke_runtime::run_state::append_transition(
    graph_state_transition_event &&event) -> void {
  if (!collect_transition_log_ && !emit_state_snapshot_events_ &&
      !emit_state_delta_events_ && !emit_runtime_message_events_ &&
      !emit_custom_events_) {
    return;
  }
  const auto node_id_iter = owner_->compiled_execution_index_.index.key_to_id.find(event.cause.node_key);
  if (node_id_iter == owner_->compiled_execution_index_.index.key_to_id.end()) {
    const auto scope = owner_->make_stream_scope(event.cause.node_key);
    detail::stream_runtime::append_state_transition(
        transition_log(), invoke_outputs_, bound_call_scope_, std::move(event),
        scope, collect_transition_log_, emit_state_snapshot_events_,
        emit_state_delta_events_, emit_runtime_message_events_,
        emit_custom_events_);
    return;
  }
  detail::stream_runtime::append_state_transition(
      transition_log(), invoke_outputs_, bound_call_scope_, std::move(event),
      runtime_stream_scope(node_id_iter->second), collect_transition_log_,
      emit_state_snapshot_events_, emit_state_delta_events_,
      emit_runtime_message_events_, emit_custom_events_);
}

inline auto detail::invoke_runtime::run_state::append_transition(
    const std::uint32_t node_id, const graph_state_transition_event &event) -> void {
  if (!collect_transition_log_ && !emit_state_snapshot_events_ &&
      !emit_state_delta_events_ && !emit_runtime_message_events_ &&
      !emit_custom_events_) {
    return;
  }
  detail::stream_runtime::append_state_transition(
      transition_log(), invoke_outputs_, bound_call_scope_, event,
      runtime_stream_scope(node_id), collect_transition_log_,
      emit_state_snapshot_events_,
      emit_state_delta_events_, emit_runtime_message_events_,
      emit_custom_events_);
}

inline auto detail::invoke_runtime::run_state::append_transition(
    const std::uint32_t node_id, graph_state_transition_event &&event) -> void {
  if (!collect_transition_log_ && !emit_state_snapshot_events_ &&
      !emit_state_delta_events_ && !emit_runtime_message_events_ &&
      !emit_custom_events_) {
    return;
  }
  detail::stream_runtime::append_state_transition(
      transition_log(), invoke_outputs_, bound_call_scope_, std::move(event),
      runtime_stream_scope(node_id), collect_transition_log_,
      emit_state_snapshot_events_,
      emit_state_delta_events_, emit_runtime_message_events_,
      emit_custom_events_);
}

inline auto detail::invoke_runtime::run_state::evaluate_resume_match(
    const std::uint32_t node_id)
    -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
  if (!context_.resume_info.has_value()) {
    return std::optional<wh::core::interrupt_signal>{};
  }
  const auto &location = runtime_node_execution_address(node_id);
  const auto match = classify_resume_target_match(*context_.resume_info, location);
  if (!match.in_resume_flow) {
    return std::optional<wh::core::interrupt_signal>{};
  }
  if (!match.should_reinterrupt) {
    if (match.match_kind == resume_target_match_kind::exact) {
      emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit, node_id, 0U);
    }
    return std::optional<wh::core::interrupt_signal>{};
  }

  auto signal = make_interrupt_signal(make_interrupt_id(), location, std::monostate{});
  if (context_.interrupt_info.has_value() &&
      !context_.interrupt_info->interrupt_id.empty()) {
    signal.interrupt_id = context_.interrupt_info->interrupt_id;
  }
  return std::optional<wh::core::interrupt_signal>{std::move(signal)};
}

inline auto detail::invoke_runtime::run_state::control_slot_id() const noexcept
    -> std::uint32_t {
  return static_cast<std::uint32_t>(owner_->compiled_execution_index_.index.nodes_by_id.size());
}

inline auto detail::invoke_runtime::run_state::request_freeze(
    const bool external_interrupt) noexcept -> void {
  freeze_requested_ = true;
  freeze_external_ = external_interrupt;
  if (external_interrupt) {
    invoke_outputs_.external_interrupt_resolution =
        detail::interrupt_runtime::resolve_external_resolution_kind(
            *detail::interrupt_runtime::freeze_external_policy_from_latch(
                 external_interrupt_policy_latch_, external_interrupt_policy_)
                 .value());
  }
}

inline auto detail::invoke_runtime::run_state::freeze_requested() const noexcept
    -> bool {
  return freeze_requested_;
}

inline auto detail::invoke_runtime::run_state::freeze_external() const noexcept
    -> bool {
  return freeze_external_;
}

inline auto detail::invoke_runtime::run_state::freeze_sender(
    const bool external_interrupt) -> graph_sender {
  const auto *policy =
      detail::interrupt_runtime::freeze_external_policy_from_latch(
          external_interrupt_policy_latch_, external_interrupt_policy_)
          .value();
  if (external_interrupt) {
    invoke_outputs_.external_interrupt_resolution =
        detail::interrupt_runtime::resolve_external_resolution_kind(*policy);
  }
  if (!((external_interrupt && policy->auto_persist_external_interrupt) ||
        (!external_interrupt && policy->manual_persist_internal_interrupt))) {
    return detail::ready_graph_sender(
        wh::core::result<graph_value>{wh::core::any(std::monostate{})});
  }
  return detail::bridge_graph_sender(
      capture_pending_inputs() |
      stdexec::then([this](wh::core::result<graph_value> captured)
                        -> wh::core::result<graph_value> {
        if (captured.has_error()) {
          return wh::core::result<graph_value>::failure(captured.error());
        }
        persist_checkpoint_best_effort();
        return wh::core::any(std::monostate{});
      }));
}

inline auto detail::invoke_runtime::run_state::flush_deferred_enqueue() -> void {
  for (const auto node_id : deferred_ready_queue_) {
    ready_queue().push_back(node_id);
    emit_debug(graph_debug_stream_event::decision_kind::enqueue, node_id,
               step_count_);
  }
  deferred_ready_queue_.clear();
}

inline auto detail::invoke_runtime::run_state::enqueue_dependents(
    const std::uint32_t source_node_id) -> void {
  for (const auto edge_id : owner_->compiled_execution_index_.index.outgoing_control(source_node_id)) {
    const auto target = owner_->compiled_execution_index_.index.indexed_edges[edge_id].to;
    if (!queued().set_if_unset(target)) {
      continue;
    }
    if (owner_->options_.eager) {
      ready_queue().push_back(target);
      emit_debug(graph_debug_stream_event::decision_kind::enqueue, target,
                 step_count_);
    } else {
      deferred_ready_queue_.push_back(target);
    }
  }
}

inline auto detail::invoke_runtime::run_state::enqueue_pregel_dependents(
    const std::uint32_t source_node_id, std::vector<std::uint32_t> &frontier,
    dynamic_bitset &frontier_queued) -> void {
  for (const auto edge_id : owner_->compiled_execution_index_.index.outgoing_control(source_node_id)) {
    const auto target = owner_->compiled_execution_index_.index.indexed_edges[edge_id].to;
    if (!frontier_queued.set_if_unset(target)) {
      continue;
    }
    frontier.push_back(target);
    emit_debug(graph_debug_stream_event::decision_kind::enqueue, target, step_count_);
  }
}

inline auto detail::invoke_runtime::run_state::check_external_interrupt_boundary()
    -> wh::core::result<bool> {
  auto boundary_state = detail::interrupt_runtime::external_interrupt_boundary_state{
      .wait_mode_active = external_interrupt_wait_mode_active_,
      .deadline = external_interrupt_deadline_,
  };
  auto handled = detail::interrupt_runtime::handle_external_boundary(
      context_, invoke_outputs_, external_interrupt_policy_latch_,
      external_interrupt_policy_, boundary_state,
      [this](const bool external_interrupt) {
        request_freeze(external_interrupt);
        return wh::core::result<void>{};
      });
  external_interrupt_wait_mode_active_ = boundary_state.wait_mode_active;
  external_interrupt_deadline_ = boundary_state.deadline;
  return handled;
}

inline auto detail::invoke_runtime::run_state::make_input_frame(
    const std::uint32_t node_id, const std::size_t step)
    -> wh::core::result<node_frame> {
  const auto *node = owner_->compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return wh::core::result<node_frame>::failure(
        wh::core::errc::not_found);
  }

  node_states()[node_id] = node_state::running;
  node_frame frame{};
  frame.stage = invoke_stage::input;
  frame.node_id = node_id;
  frame.cause = graph_state_cause{
      .run_id = run_id_,
      .step = step,
      .node_key = owner_->compiled_execution_index_.index.id_to_key[node_id],
  };
  frame.node = node;
  if (node_id < resolved_state_handlers_.size()) {
    frame.state_handlers = resolved_state_handlers_[node_id];
  }
  return frame;
}

inline auto detail::invoke_runtime::run_state::begin_state_pre(
    node_frame &&frame, graph_value input)
    -> wh::core::result<state_step> {
  if (frame.node == nullptr) {
    return wh::core::result<state_step>::failure(
        wh::core::errc::not_found);
  }

  const auto &node_key = owner_->compiled_execution_index_.index.id_to_key[frame.node_id];
  auto resume_interrupt = evaluate_resume_match(frame.node_id);
  if (resume_interrupt.has_error()) {
    persist_checkpoint_best_effort();
    return wh::core::result<state_step>::failure(
        resume_interrupt.error());
  }
  if (resume_interrupt.value().has_value()) {
    context_.interrupt_info = wh::compose::to_interrupt_context(
        std::move(resume_interrupt.value().value()));
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
               frame.node_id, frame.cause.step);
    rerun_state_.store(frame.node_id, std::move(input));
    node_states()[frame.node_id] = node_state::pending;
    request_freeze(false);
    return wh::core::result<state_step>::failure(
        wh::core::errc::canceled);
  }

  auto pre_interrupt = owner_->evaluate_interrupt_hook(
      context_, invoke_config_.interrupt_pre_hook, node_key, input);
  if (pre_interrupt.has_error()) {
    persist_checkpoint_best_effort();
    return wh::core::result<state_step>::failure(
        pre_interrupt.error());
  }
  if (pre_interrupt.value().has_value()) {
    context_.interrupt_info = wh::compose::to_interrupt_context(
        std::move(pre_interrupt.value().value()));
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
               frame.node_id, frame.cause.step);
    rerun_state_.store(frame.node_id, std::move(input));
    node_states()[frame.node_id] = node_state::pending;
    request_freeze(false);
    return wh::core::result<state_step>::failure(
        wh::core::errc::canceled);
  }

  auto node_local_state = detail::process_runtime::acquire_node_local_process_state(
      node_local_process_states_, frame.node_id, process_state_);
  if (node_local_state.has_error()) {
    persist_checkpoint_best_effort();
    return wh::core::result<state_step>::failure(
        node_local_state.error());
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
    return wh::core::result<state_step>::failure(
        node_local_ref.error());
  }

  frame.node_scope.path = runtime_node_path(frame.node_id);
  frame.node_scope.local_process_state = std::addressof(node_local_ref.value().get());

  const auto should_skip_pre_handler =
      skip_state_pre_handlers_ && rerun_state_.restored(frame.node_id);
  if (!should_skip_pre_handler) {
    if (detail::state_runtime::needs_async_phase(
            frame.state_handlers, input, detail::state_runtime::state_phase::pre)) {
      if (!graph_scheduler_.has_value()) {
        persist_checkpoint_best_effort();
        return wh::core::result<state_step>::failure(
            wh::core::errc::contract_violation);
      }
      auto sender = owner_->apply_state_phase_async(
          context_, frame.state_handlers,
          detail::state_runtime::state_phase::pre, node_key, frame.cause,
          node_local_ref.value().get(), std::move(input), frame.node_scope.path,
          invoke_outputs_, *graph_scheduler_);
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
        node_key, frame.cause, node_local_ref.value().get(), input,
        frame.node_scope.path, invoke_outputs_);
    if (pre_state.has_error()) {
      owner_->publish_node_run_error(
          invoke_outputs_, frame.node_scope.path, frame.node_id,
          pre_state.error(), "node pre-state handler failed");
      state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                          pre_state.error());
      append_transition(frame.node_id, graph_state_transition_event{
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
    node_frame &&frame, graph_value input)
    -> wh::core::result<state_step> {
  if (!frame.input_lowering.has_value()) {
    return state_step{
        .frame = std::move(frame),
        .payload = std::move(input),
        .sender = std::nullopt,
    };
  }

  auto *reader = wh::core::any_cast<graph_stream_reader>(&input);
  if (reader == nullptr) {
    return wh::core::result<state_step>::failure(
        wh::core::errc::type_mismatch);
  }

  auto lowering = std::move(*frame.input_lowering);
  frame.input_lowering.reset();
  if (!graph_scheduler_.has_value()) {
    return wh::core::result<state_step>::failure(
        wh::core::errc::contract_violation);
  }
  frame.stage = invoke_stage::prepare;
  return state_step{
      .frame = std::move(frame),
      .payload = {},
      .sender = owner_->lower_reader(std::move(*reader), std::move(lowering),
                                     context_, *graph_scheduler_),
  };
}

inline auto detail::invoke_runtime::run_state::should_retain_input(
    const node_frame &frame) const noexcept -> bool {
  return retain_inputs_ || frame.retry_budget > 0U;
}

inline auto detail::invoke_runtime::run_state::finalize_node_frame(
    node_frame &&frame, graph_value input)
    -> wh::core::result<node_frame> {
  state_table_.update(frame.node_id, graph_node_lifecycle_state::running, 0U,
                      std::nullopt);
  append_transition(frame.node_id, graph_state_transition_event{
      .kind = graph_state_transition_kind::node_enter,
      .cause = frame.cause,
      .lifecycle = graph_node_lifecycle_state::running,
  });

  const auto node_retry_budget = owner_->resolve_node_retry_budget(frame.node_id);
  const auto node_timeout_budget =
      owner_->resolve_node_timeout_budget(frame.node_id);
  const auto effective_parallel_gate =
      std::min(owner_->options_.max_parallel_nodes,
               owner_->resolve_node_parallel_gate(frame.node_id));
  if (effective_parallel_gate == 0U) {
    frame.node_local_scope.release(node_local_process_states_);
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        wh::core::errc::contract_violation);
    append_transition(frame.node_id, graph_state_transition_event{
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
  frame.node_runtime = node_runtime{
      .parallel_gate = effective_parallel_gate,
      .call_options = nullptr,
  };
  return frame;
}

inline auto detail::invoke_runtime::run_state::begin_state_post(
    node_frame &&frame, graph_value output)
    -> wh::core::result<state_step> {
  if (detail::state_runtime::needs_async_phase(
          frame.state_handlers, output, detail::state_runtime::state_phase::post)) {
    if (!graph_scheduler_.has_value()) {
      return wh::core::result<state_step>::failure(
          wh::core::errc::contract_violation);
    }
    auto sender = owner_->apply_state_phase_async(
        context_, frame.state_handlers,
        detail::state_runtime::state_phase::post, frame.cause.node_key,
        frame.cause, *frame.node_scope.local_process_state, std::move(output),
        frame.node_scope.path, invoke_outputs_, *graph_scheduler_);
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
      output, frame.node_scope.path, invoke_outputs_);
  if (post_state.has_error()) {
    owner_->publish_node_run_error(invoke_outputs_, frame.node_scope.path,
                                   frame.node_id, post_state.error(),
                                   "node post-state handler failed");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        post_state.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
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
  owner_->publish_node_run_error(invoke_outputs_, frame.node_scope.path,
                                 frame.node_id, code, message);
  state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U, code);
  append_transition(frame.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::node_fail,
                                     .cause = frame.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::failed,
                                 });
  persist_checkpoint_best_effort();
  frame.node_local_scope.release(node_local_process_states_);
  return wh::core::result<void>::failure(code);
}

inline auto detail::invoke_runtime::run_state::bind_node_runtime_call_options(
    node_frame &frame,
    const graph_call_scope &bound_call_options,
    run_state *state) noexcept -> void {
  frame.node_runtime.call_options = std::addressof(bound_call_options);
  frame.node_runtime.path = std::addressof(frame.node_scope.path);
  frame.node_runtime.graph_scheduler =
      state->graph_scheduler_.has_value()
          ? std::addressof(*state->graph_scheduler_)
          : nullptr;
  frame.node_runtime.local_process_state = frame.node_scope.local_process_state;
  frame.node_scope.component_options =
      state->resolved_component_options_.empty()
          ? nullptr
          : std::addressof(state->resolved_component_options_[frame.node_id]);
  frame.node_scope.observation =
      state->resolved_node_observations_.empty()
          ? nullptr
          : std::addressof(state->resolved_node_observations_[frame.node_id]);
  frame.node_scope.trace = state->next_node_trace(frame.node_id);
  frame.node_runtime.component_options = frame.node_scope.component_options;
  frame.node_runtime.observation = frame.node_scope.observation;
  frame.node_runtime.trace = std::addressof(frame.node_scope.trace);
  frame.node_runtime.invoke_outputs = std::addressof(state->invoke_outputs_);
  frame.node_runtime.nested_entry = state->nested_graph_entry();
}

inline auto detail::invoke_runtime::run_state::start_nested_from_runtime(
    const void *state_ptr, const graph &nested_graph,
    wh::core::run_context &context,
    graph_value &input, const graph_call_scope *call_options,
    const node_path *path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const graph_node_trace *parent_trace) -> graph_sender {
  const auto *state =
      static_cast<const run_state *>(state_ptr);
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
  return detail::start_scoped_graph(
      nested_graph, context, input, std::addressof(nested_call_scope),
      path_prefix, parent_process_state, nested_outputs,
      *state->graph_scheduler_, state);
}

inline auto detail::invoke_runtime::run_state::nested_graph_entry() const noexcept
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
    detail::runtime_state::invoke_outputs &outputs, const std::string_view node_key,
    const std::size_t attempt, const std::chrono::milliseconds timeout_budget,
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
    detail::runtime_state::invoke_outputs &outputs,
    const node_frame &frame,
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
  auto normalized =
      ::wh::compose::detail::normalize_graph_sender(std::forward<sender_t>(sender));
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
    run_state *state, node_frame &frame,
    retry_fn_t retry_fn) -> wh::core::result<graph_value> {
  while (true) {
    bind_node_runtime_call_options(frame, bound_call_options, state);
    const auto attempt_start = std::chrono::steady_clock::now();
    auto executed =
        run_compiled_sync_node(node, input_value, context, frame.node_runtime);
    executed = apply_node_timeout(state->invoke_outputs_, frame, attempt_start,
                                  std::move(executed));
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
    run_state *state, node_frame &frame)
    -> graph_sender {
  bind_node_runtime_call_options(frame, bound_call_options, state);
  const auto attempt_start = std::chrono::steady_clock::now();
  return make_async_timed_node_sender(
      run_compiled_async_node(node, input_value, context, frame.node_runtime),
      state->invoke_outputs_, frame, attempt_start);
}

template <typename enqueue_fn_t>
inline auto detail::invoke_runtime::run_state::commit_node_output(
    node_frame &&frame, graph_value node_output,
    enqueue_fn_t &&enqueue_fn) -> wh::core::result<void> {
  auto release_node_local_state = [&]() noexcept {
    frame.node_local_scope.release(node_local_process_states_);
  };
  auto stored_output =
      owner_->store_node_output(frame.node_id, scratch_, std::move(node_output));
  if (stored_output.has_error()) {
    owner_->publish_node_run_error(invoke_outputs_, frame.node_scope.path,
                                   frame.node_id, stored_output.error(),
                                   "node output contract mismatch");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        stored_output.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
                                   });
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(stored_output.error());
  }
  auto output_view = owner_->view_node_output(frame.node_id, scratch_);
  if (output_view.has_error()) {
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(output_view.error());
  }
  auto resolved_branch = owner_->evaluate_branch_indexed(
      frame.node_id, output_view.value(), context_, bound_call_scope_);
  if (resolved_branch.has_error()) {
    owner_->publish_node_run_error(invoke_outputs_, frame.node_scope.path,
                                   frame.node_id, resolved_branch.error(),
                                   "branch selector failed");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        resolved_branch.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
                                   });
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(resolved_branch.error());
  }
  auto branch_committed = owner_->commit_branch_selection(
      frame.node_id, std::move(resolved_branch).value(), scratch_, invoke_config_);
  if (branch_committed.has_error()) {
    owner_->publish_node_run_error(invoke_outputs_, frame.node_scope.path,
                                   frame.node_id, branch_committed.error(),
                                   "branch commit failed");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        branch_committed.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
                                   });
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(branch_committed.error());
  }
  node_states()[frame.node_id] = node_state::executed;
  auto refreshed_streams = owner_->refresh_source_readers(
      frame.node_id, scratch_, node_states(), branch_states(), context_);
  if (refreshed_streams.has_error()) {
    owner_->publish_node_run_error(invoke_outputs_, frame.node_scope.path,
                                   frame.node_id, refreshed_streams.error(),
                                   "merged stream refresh failed");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        refreshed_streams.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
                                   });
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(refreshed_streams.error());
  }
  state_table_.update(frame.node_id, graph_node_lifecycle_state::completed, 0U,
                      std::nullopt);
  append_transition(frame.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::route_commit,
                                     .cause = frame.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::completed,
                                 });
  append_transition(frame.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::node_leave,
                                     .cause = frame.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::completed,
                                 });
  auto post_output = owner_->view_node_output(frame.node_id, scratch_);
  if (post_output.has_error()) {
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(post_output.error());
  }
  auto post_interrupt = owner_->evaluate_interrupt_hook(
      context_, invoke_config_.interrupt_post_hook, frame.cause.node_key,
      post_output.value());
  if (post_interrupt.has_error()) {
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(post_interrupt.error());
  }
  if (post_interrupt.value().has_value()) {
    context_.interrupt_info =
        wh::compose::to_interrupt_context(std::move(post_interrupt).value().value());
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit, frame.node_id,
               step_count_);
    request_freeze(false);
    release_node_local_state();
    return wh::core::result<void>::failure(wh::core::errc::canceled);
  }

  if (!freeze_requested_) {
    enqueue_fn(frame.node_id);
  }
  release_node_local_state();
  return {};
}

inline auto detail::invoke_runtime::run_state::commit_pregel_skip_action(
    const pregel_action &action, std::vector<std::uint32_t> &next_frontier,
    dynamic_bitset &next_frontier_queued) -> wh::core::result<void> {
  node_states()[action.node_id] = node_state::skipped;
  state_table_.update(action.node_id, graph_node_lifecycle_state::skipped, 0U,
                      std::nullopt);
  append_transition(action.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::node_skip,
                                     .cause = action.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::skipped,
                                 });
  emit_debug(graph_debug_stream_event::decision_kind::skipped, action.node_id,
             action.cause.step);
  auto refreshed_streams = owner_->refresh_source_readers(
      action.node_id, scratch_, node_states(), branch_states(), context_);
  if (refreshed_streams.has_error()) {
    return refreshed_streams;
  }
  enqueue_pregel_dependents(action.node_id, next_frontier, next_frontier_queued);
  return {};
}

inline auto detail::invoke_runtime::run_state::finish_graph_status()
    -> wh::core::result<graph_value> {
  if (node_states()[owner_->compiled_execution_index_.index.end_id] != node_state::executed) {
    owner_->publish_graph_run_error(
        invoke_outputs_, runtime_node_path(owner_->compiled_execution_index_.index.end_id),
        owner_->compiled_execution_index_.index.id_to_key[owner_->compiled_execution_index_.index.end_id],
        compose_error_phase::execute, wh::core::errc::contract_violation,
        "end node was not executed");
    owner_->publish_last_completed_nodes(invoke_outputs_, node_states());
    persist_checkpoint_best_effort();
    return wh::core::result<graph_value>::failure(
        wh::core::errc::contract_violation);
  }
  if (!output_valid().test(owner_->compiled_execution_index_.index.end_id)) {
    owner_->publish_graph_run_error(
        invoke_outputs_, runtime_node_path(owner_->compiled_execution_index_.index.end_id),
        owner_->compiled_execution_index_.index.id_to_key[owner_->compiled_execution_index_.index.end_id],
        compose_error_phase::execute, wh::core::errc::not_found,
        "end node output not found");
    persist_checkpoint_best_effort();
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  auto final_output = owner_->take_node_output(owner_->compiled_execution_index_.index.end_id,
                                               scratch_);
  if (final_output.has_value()) {
    output_valid().clear(owner_->compiled_execution_index_.index.end_id);
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
