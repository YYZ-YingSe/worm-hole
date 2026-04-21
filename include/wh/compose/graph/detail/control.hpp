// Defines graph runtime control, publish, restore, and validation helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {
inline auto graph::next_invoke_run_id() noexcept -> std::uint64_t {
  static std::atomic<std::uint64_t> sequence{1U};
  return sequence.fetch_add(1U, std::memory_order_relaxed);
}

inline constexpr auto
graph::should_wrap_as_node_run_error(const wh::core::error_code code) noexcept
    -> bool {
  return code != wh::core::errc::canceled;
}

inline auto
graph::validate_call_scope_for_runtime(const graph_call_scope &call_scope) const
    -> wh::core::result<void> {
  const auto &call_options = call_scope.options();
  for (const auto &[option_key, _] : call_options.component_defaults) {
    if (option_key.find('/') != std::string::npos) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
  }

  if (call_scope.prefix().empty()) {
    for (const auto &node_key : call_options.designated_top_level_nodes) {
      if (node_key.empty()) {
        return wh::core::result<void>::failure(
            wh::core::errc::invalid_argument);
      }
      if (!core().compiled_execution_index_.index.key_to_id.contains(
              node_key)) {
        return wh::core::result<void>::failure(wh::core::errc::not_found);
      }
    }
  }

  auto validate_path = [&](const node_path &path) -> wh::core::result<void> {
    const auto segments = path.segments();
    if (segments.empty() ||
        !core().compiled_execution_index_.index.key_to_id.contains(
            std::string_view{segments.front()})) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return {};
  };

  auto validate_scoped_path =
      [&](const node_path &path) -> wh::core::result<void> {
    auto relative = call_scope.relative_path(path);
    if (!relative.has_value()) {
      return {};
    }
    if (relative->empty()) {
      return call_scope.prefix().empty() ? wh::core::result<void>::failure(
                                               wh::core::errc::invalid_argument)
                                         : wh::core::result<void>{};
    }
    return validate_path(*relative);
  };

  for (const auto &path : call_options.designated_node_paths) {
    auto validated = validate_scoped_path(path);
    if (validated.has_error()) {
      return validated;
    }
  }
  for (const auto &targeted : call_options.component_overrides) {
    auto validated = validate_scoped_path(targeted.path);
    if (validated.has_error()) {
      return validated;
    }
    for (const auto &[option_key, option_value] : targeted.values) {
      if (option_key.find('/') != std::string::npos) {
        return wh::core::result<void>::failure(
            wh::core::errc::invalid_argument);
      }
      const auto default_iter =
          call_options.component_defaults.find(option_key);
      if (default_iter == call_options.component_defaults.end()) {
        continue;
      }
      if (default_iter->second.key() != option_value.key()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
    }
  }
  for (const auto &observation : call_options.node_observations) {
    auto validated = validate_scoped_path(observation.path);
    if (validated.has_error()) {
      return validated;
    }
  }
  for (const auto &observer : call_options.node_path_debug_observers) {
    if (!observer.callback) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    auto validated = validate_scoped_path(observer.path);
    if (validated.has_error()) {
      return validated;
    }
  }
  return {};
}

inline auto graph::make_node_designation_path(const std::uint32_t node_id) const
    -> node_path {
  const std::array<std::string_view, 1U> segments{
      core().compiled_execution_index_.index.id_to_key[node_id]};
  return make_node_path(
      std::span<const std::string_view>{segments.data(), segments.size()});
}

inline auto graph::make_runtime_node_path(const node_path &prefix,
                                          const std::uint32_t node_id) const
    -> node_path {
  if (!prefix.empty()) {
    const auto segments = prefix.segments();
    const auto &node_key =
        core().compiled_execution_index_.index.id_to_key[node_id];
    if (!segments.empty() && segments.back() == node_key) {
      return prefix;
    }
    return prefix.append(node_key);
  }
  return make_node_designation_path(node_id);
}

inline auto
graph::publish_node_run_error(detail::runtime_state::invoke_outputs &outputs,
                              const node_path &runtime_path,
                              const std::uint32_t node_id,
                              const wh::core::error_code code,
                              const std::string_view message) const -> void {
  const auto &node_key =
      core().compiled_execution_index_.index.id_to_key[node_id];
  outputs.node_run_error = graph_node_run_error_detail{
      .path = runtime_path,
      .node = node_key,
      .code = code,
      .raw_error = code,
      .message = std::string{message},
  };
}

inline auto graph::publish_graph_run_error(
    detail::runtime_state::invoke_outputs &outputs,
    const std::optional<node_path> &runtime_path,
    const std::string_view node_key, const compose_error_phase phase,
    const wh::core::error_code code, const std::string_view message,
    const std::optional<wh::core::error_code> raw_error) const -> void {
  graph_run_error_detail detail{};
  detail.phase = phase;
  detail.code = code;
  detail.raw_error = raw_error.has_value() ? raw_error : std::optional{code};
  detail.message = std::string{message};
  detail.path = runtime_path;
  detail.node = std::string{node_key};
  outputs.graph_run_error = std::move(detail);
}

inline auto graph::publish_stream_read_error(
    detail::runtime_state::invoke_outputs &outputs, node_path runtime_path,
    const std::string_view node_key, const wh::core::error_code code,
    const std::string_view message) const -> void {
  outputs.stream_read_error = graph_new_stream_read_error_detail{
      .path = std::move(runtime_path),
      .node = std::string{node_key},
      .code = code,
      .raw_error = code,
      .message = std::string{message},
  };
}

inline auto
graph::make_node_execution_address(const node_path &runtime_path) const
    -> wh::core::address {
  std::vector<std::string_view> segments{};
  segments.reserve(runtime_path.size() + 1U);
  segments.push_back(core().options_.name);
  for (const auto &segment : runtime_path.segments()) {
    segments.push_back(segment);
  }
  return wh::core::make_address(
      std::span<const std::string_view>{segments.data(), segments.size()});
}

inline auto
graph::has_descendant_designation_target(const graph_call_scope &call_options,
                                         const node_path &path) -> bool {
  const auto absolute = call_options.absolute_path(path);
  return std::ranges::any_of(call_options.options().designated_node_paths,
                             [&](const node_path &candidate) {
                               return candidate.starts_with(absolute);
                             });
}

inline auto
graph::has_active_designation(const graph_call_scope &options) noexcept
    -> bool {
  return (options.prefix().empty() &&
          !options.options().designated_top_level_nodes.empty()) ||
         !options.options().designated_node_paths.empty();
}

inline auto
graph::is_node_designated(const std::uint32_t node_id,
                          const graph_call_scope &call_options) const -> bool {
  if (node_id == core().compiled_execution_index_.index.start_id ||
      node_id == core().compiled_execution_index_.index.end_id) {
    return true;
  }
  if (!has_active_designation(call_options)) {
    return true;
  }
  const auto designation_path = make_node_designation_path(node_id);
  if (is_graph_node_designated(
          call_options,
          core().compiled_execution_index_.index.id_to_key[node_id],
          designation_path)) {
    return true;
  }
  return has_descendant_designation_target(call_options, designation_path);
}

inline auto graph::emit_debug_stream_event(
    wh::core::run_context &context,
    detail::runtime_state::invoke_outputs &outputs,
    const graph_call_scope &options,
    const graph_debug_stream_event::decision_kind decision,
    const std::uint32_t node_id, const node_path &runtime_path,
    const std::size_t step) const -> void {
  if (!should_emit_graph_debug_event(options)) {
    return;
  }
  const auto event = graph_debug_stream_event{
      .decision = decision,
      .node_key = core().compiled_execution_index_.index.id_to_key[node_id],
      .path = runtime_path,
      .step = step,
  };
  detail::stream_runtime::emit_debug_event(
      context, outputs, options, event,
      make_graph_event_scope(core().options_.name, event.node_key, event.path));
}

inline auto graph::make_stream_scope(const std::string_view node_key) const
    -> graph_event_scope {
  const std::array<std::string_view, 1U> segments{node_key};
  const auto path = make_node_path(
      std::span<const std::string_view>{segments.data(), segments.size()});
  return make_graph_event_scope(core().options_.name, node_key, path);
}

inline auto graph::make_stream_scope(const std::string_view node_key,
                                     const node_path &runtime_path) const
    -> graph_event_scope {
  return make_graph_event_scope(core().options_.name, node_key, runtime_path);
}

inline auto
graph::append_state_transition(detail::runtime_state::invoke_outputs &outputs,
                               const graph_call_scope &options,
                               const graph_state_transition_event &event,
                               const node_path &runtime_path) const -> void {
  const auto emit_state_snapshot_events = has_graph_stream_subscription(
      options, graph_stream_channel_kind::state_snapshot);
  const auto emit_state_delta_events = has_graph_stream_subscription(
      options, graph_stream_channel_kind::state_delta);
  const auto emit_runtime_message_events = has_graph_stream_subscription(
      options, graph_stream_channel_kind::message);
  const auto emit_custom_events = std::ranges::any_of(
      options.options().stream_subscriptions,
      [](const graph_stream_subscription &subscription) {
        return subscription.enabled &&
               subscription.kind == graph_stream_channel_kind::custom &&
               !subscription.custom_channel.empty();
      });
  detail::stream_runtime::append_state_transition(
      outputs.transition_log, outputs, options, event,
      make_stream_scope(event.cause.node_key, runtime_path), true,
      emit_state_snapshot_events, emit_state_delta_events,
      emit_runtime_message_events, emit_custom_events);
}

inline auto
graph::append_state_transition(detail::runtime_state::invoke_outputs &outputs,
                               const graph_call_scope &options,
                               graph_state_transition_event &&event,
                               const node_path &runtime_path) const -> void {
  const auto emit_state_snapshot_events = has_graph_stream_subscription(
      options, graph_stream_channel_kind::state_snapshot);
  const auto emit_state_delta_events = has_graph_stream_subscription(
      options, graph_stream_channel_kind::state_delta);
  const auto emit_runtime_message_events = has_graph_stream_subscription(
      options, graph_stream_channel_kind::message);
  const auto emit_custom_events = std::ranges::any_of(
      options.options().stream_subscriptions,
      [](const graph_stream_subscription &subscription) {
        return subscription.enabled &&
               subscription.kind == graph_stream_channel_kind::custom &&
               !subscription.custom_channel.empty();
      });
  detail::stream_runtime::append_state_transition(
      outputs.transition_log, outputs, options, std::move(event),
      make_stream_scope(event.cause.node_key, runtime_path), true,
      emit_state_snapshot_events, emit_state_delta_events,
      emit_runtime_message_events, emit_custom_events);
}

inline auto graph::evaluate_interrupt_hook(
    wh::core::run_context &context, const graph_interrupt_node_hook &hook,
    const std::string_view node_key, const graph_value &payload) const
    -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
  return detail::interrupt_runtime::evaluate_hook(context, hook, node_key,
                                                  payload);
}

inline auto
graph::make_missing_pending_input_default(const node_contract contract)
    -> wh::core::result<graph_value> {
  if (contract == node_contract::stream) {
    auto [writer, reader] = make_graph_stream(1U);
    auto closed = writer.close();
    if (closed.has_error()) {
      return wh::core::result<graph_value>::failure(closed.error());
    }
    return wh::core::any(std::move(reader));
  }
  return wh::core::any(std::monostate{});
}

inline auto
graph::resolve_missing_pending_input(const node_contract input_contract) const
    -> wh::core::result<graph_value> {
  return make_missing_pending_input_default(input_contract);
}

inline auto graph::apply_runtime_resume_controls(
    wh::core::run_context &context,
    const detail::runtime_state::invoke_config &config) const
    -> wh::core::result<void> {
  return detail::interrupt_runtime::apply_runtime_resume_controls(context,
                                                                  config);
}

inline auto graph::prepare_restore_checkpoint(
    wh::core::run_context &context,
    const detail::runtime_state::invoke_config &config,
    const detail::checkpoint_runtime::restore_scope scope,
    const node_path &runtime_path,
    detail::runtime_state::invoke_outputs &outputs,
    forwarded_checkpoint_map &forwarded_checkpoints) const
    -> wh::core::result<
        std::optional<detail::checkpoint_runtime::prepared_restore>> {
  return detail::checkpoint_runtime::prepare_restore(
      context, scope, core().options_.name, runtime_path, core().restore_shape_,
      core().compiled_execution_index_.index.id_to_key,
      core().compiled_execution_index_.index.indexed_edges,
      core().compiled_execution_index_.index.end_id, config, outputs,
      forwarded_checkpoints);
}

inline auto graph::maybe_persist_checkpoint(
    wh::core::run_context &context, checkpoint_state checkpoint,
    const detail::runtime_state::invoke_config &config,
    detail::runtime_state::invoke_outputs &outputs) const
    -> wh::core::result<void> {
  return detail::checkpoint_runtime::maybe_persist(
      context, std::move(checkpoint), core().compiled_execution_index_.index.id_to_key,
      core().compiled_execution_index_.index.indexed_edges,
      core().compiled_execution_index_.index.end_id, config, outputs);
}

} // namespace wh::compose
