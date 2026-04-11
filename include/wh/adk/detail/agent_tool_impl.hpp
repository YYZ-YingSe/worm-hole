// Defines the internal bridge runtime and out-of-line member implementations
// for the public agent_tool surface.
#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/adk/detail/event_message_stream_reader.hpp"
#include "wh/adk/detail/history_request.hpp"
#include "wh/adk/detail/live_event_bridge.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/core/any.hpp"
#include "wh/core/json.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/detail/inline_drive_loop.hpp"
#include "wh/core/stdexec/detail/receiver_completion.hpp"
#include "wh/core/stdexec/detail/single_completion_slot.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace wh::adk::detail {

inline constexpr std::string_view agent_tool_interrupt_id_prefix = "tool:";
inline constexpr std::string_view agent_tool_interrupt_default_suffix = "interrupt";
inline constexpr std::string_view agent_tool_interrupt_reason = "agent tool interrupted";
inline constexpr std::string_view agent_tool_request_json_key = "request";
inline constexpr std::string_view agent_tool_bridge_failed_message = "agent tool bridge failed";
inline constexpr std::string_view agent_tool_history_json_schema =
    R"({"type":"object","properties":{"messages":{"type":"array","items":{"type":"object"}}},"required":["messages"]})";

/// Frozen runtime bundle captured by compose tool-entry lambdas.
struct agent_tool_runtime {
  /// Stable public tool name.
  std::string tool_name{};
  /// Stable bound child-agent name.
  std::string agent_name{};
  /// Frozen input mapping mode.
  agent_tool_input_mode input_mode{agent_tool_input_mode::request};
  /// True forwards child internal events after boundary filtering.
  bool forward_internal_events{false};
  /// Frozen child-agent execution entrypoint.
  agent_tool_runner runner{nullptr};
};

/// Copyable custom-event payload retained inside bridge checkpoint state.
struct agent_tool_custom_event_record {
  /// Stable custom event name.
  std::string name{};
  /// Copyable custom payload retained when available.
  wh::core::any payload{};
};

/// Copyable error payload retained inside bridge checkpoint state.
struct agent_tool_error_event_record {
  /// Stable error code emitted across the bridge boundary.
  wh::core::error_code code{};
  /// Human-readable bridge-visible error text.
  std::string message{};
  /// Copyable detail payload retained when available.
  wh::core::any detail{};
};

/// Copyable bridge-visible event record retained across checkpoint/resume.
struct agent_tool_event_record {
  /// Payload snapshot after stripping move-only stream handles.
  std::variant<wh::schema::message, agent_tool_custom_event_record, agent_tool_error_event_record>
      payload{};
  /// Event metadata already normalized to the tool boundary.
  event_metadata metadata{};
};

/// Copyable child-interrupt snapshot retained by the tool bridge.
struct agent_tool_child_interrupt {
  /// Stable child interrupt identifier.
  std::string interrupt_id{};
  /// Exact child resume location inside the nested run path.
  wh::core::address location{};
  /// Child interrupt state retained for resume replay when copyable.
  wh::core::any state{};
  /// Child layer payload preserved for diagnostics when copyable.
  wh::core::any layer_payload{};
  /// Parent address chain copied from the child interrupt.
  std::vector<wh::core::address> parent_locations{};
  /// Human-readable child trigger reason.
  std::string trigger_reason{};
};

/// Checkpoint-safe bridge-local progress snapshot.
struct agent_tool_checkpoint_state {
  /// Boundary-visible non-interrupt events already materialized before freeze.
  std::vector<agent_tool_event_record> events{};
  /// Ordered text chunks already extracted before freeze.
  std::vector<std::string> output_chunks{};
  /// Last visible message snapshot, when available.
  std::optional<wh::schema::message> final_message{};
};

/// Interrupt state stored at the tool boundary and restored on resume.
struct agent_tool_interrupt_state {
  /// Bridge-local progress that must survive checkpoint/load/resume.
  agent_tool_checkpoint_state checkpoint{};
  /// Child interrupt snapshot used to rebuild nested resume mapping.
  std::optional<agent_tool_child_interrupt> child_interrupt{};
};

} // namespace wh::adk::detail

namespace wh::core {

template <> struct any_owned_traits<wh::adk::detail::agent_tool_interrupt_state>;

} // namespace wh::core

namespace wh::adk::detail {

/// Resume projection extracted from one exact tool-boundary resume target.
struct agent_tool_resume_projection {
  /// Exact outer interrupt id consumed for this tool-boundary resume.
  std::string outer_interrupt_id{};
  /// Resume patch injected by the caller for the outer tool boundary.
  wh::compose::resume_patch patch{};
  /// Restored bridge state saved by the previous interrupted run.
  agent_tool_interrupt_state state{};
};

/// Owned tool-call scope fields retained after the public borrowed scope
/// object goes out of scope.
struct agent_tool_scope_snapshot {
  /// Exact outer tool-boundary location.
  wh::core::address location{};
  /// Stable tool call id used when synthesizing interrupt ids.
  std::string call_id{};
};

/// Shared pre-run lowering bundle reused by eager and live tool paths.
struct agent_tool_run_setup {
  /// Lowered child run request after authored input projection.
  wh::adk::run_request request{};
  /// Optional resume projection consumed for this outer tool boundary.
  std::optional<agent_tool_resume_projection> projection{};
  /// Previous outer interrupt temporarily cleared before entering the child.
  std::optional<wh::core::interrupt_context> saved_outer_interrupt{};
  /// True when the previous outer interrupt matched this tool boundary.
  bool cleared_outer_interrupt{false};
};

/// Intermediate summary accumulated while projecting one bridge run.
struct agent_tool_output_summary {
  /// Checkpoint-safe boundary-visible event records accumulated in lockstep.
  std::vector<agent_tool_event_record> checkpoint_events{};
  /// Ordered text chunks extracted from child message events.
  std::vector<std::string> text_chunks{};
  /// Final observed message, when any.
  std::optional<wh::schema::message> final_message{};
  /// Terminal error observed inside the child event stream.
  std::optional<wh::core::error_code> final_error{};
  /// Latest interrupt hint observed at the tool boundary, when any.
  std::optional<agent_tool_child_interrupt> child_interrupt{};
  /// True when one interrupt action survived the tool boundary.
  bool interrupted{false};
};

/// Internal-only accessor that materializes runtime state from the public shell.
struct agent_tool_access {
  [[nodiscard]] static auto make_runtime(agent_tool &tool) -> wh::core::result<agent_tool_runtime> {
    auto frozen = tool.freeze();
    if (frozen.has_error()) {
      return wh::core::result<agent_tool_runtime>::failure(frozen.error());
    }
    if (!static_cast<bool>(tool.runner_)) {
      return wh::core::result<agent_tool_runtime>::failure(wh::core::errc::not_found);
    }
    return agent_tool_runtime{
        .tool_name = tool.name_,
        .agent_name = std::string{tool.bound_agent_.name()},
        .input_mode = tool.input_mode_,
        .forward_internal_events = tool.forward_internal_events_,
        .runner = tool.runner_,
    };
  }
};

[[nodiscard]] inline auto make_user_message(std::string text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

[[nodiscard]] inline auto render_message_text(const wh::schema::message &message) -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part); typed != nullptr) {
      text.append(typed->text);
    }
  }
  return text;
}

[[nodiscard]] inline auto make_agent_tool_scope_snapshot(const wh::tool::call_scope &scope)
    -> agent_tool_scope_snapshot {
  return agent_tool_scope_snapshot{
      .location = scope.location(),
      .call_id = std::string{scope.call_id},
  };
}

[[nodiscard]] inline auto normalize_child_metadata(const agent_tool_runtime &runtime,
                                                   const agent_tool_scope_snapshot &scope,
                                                   event_metadata metadata) -> event_metadata {
  if (metadata.run_path.empty()) {
    metadata.run_path = run_path{{"agent", runtime.agent_name}};
  }
  metadata.run_path = append_run_path_prefix(scope.location, metadata.run_path);
  if (metadata.agent_name.empty()) {
    metadata.agent_name = runtime.agent_name;
  }
  if (metadata.tool_name.empty()) {
    metadata.tool_name = runtime.tool_name;
  }
  return metadata;
}

[[nodiscard]] inline auto default_tool_metadata(const agent_tool_runtime &runtime,
                                                const agent_tool_scope_snapshot &scope)
    -> event_metadata {
  return normalize_child_metadata(runtime, scope, event_metadata{});
}

// Best-effort bridge-state capture: unsupported payloads degrade to empty.
[[nodiscard]] inline auto capture_bridge_state_any_or_empty(const wh::core::any &payload)
    -> wh::core::any {
  auto owned = wh::core::into_owned(payload);
  if (owned.has_error()) {
    return {};
  }
  return std::move(owned).value();
}

// Best-effort bridge-state capture: unsupported payloads degrade to empty.
[[nodiscard]] inline auto capture_bridge_state_any_or_empty(wh::core::any &&payload)
    -> wh::core::any {
  auto owned = wh::core::into_owned(std::move(payload));
  if (owned.has_error()) {
    return {};
  }
  return std::move(owned).value();
}

// Best-effort bridge-state capture: unsupported attributes are dropped.
[[nodiscard]] inline auto capture_bridge_metadata(const event_metadata &metadata)
    -> event_metadata {
  event_metadata owned{};
  owned.run_path = metadata.run_path;
  owned.agent_name = metadata.agent_name;
  owned.tool_name = metadata.tool_name;
  owned.attributes.reserve(metadata.attributes.size());
  for (const auto &[key, value] : metadata.attributes) {
    auto owned_value = wh::core::into_owned(value);
    if (owned_value.has_error()) {
      continue;
    }
    owned.attributes.emplace(key, std::move(owned_value).value());
  }
  return owned;
}

// Best-effort bridge-state capture: unsupported attributes are dropped.
[[nodiscard]] inline auto capture_bridge_metadata(event_metadata &&metadata) -> event_metadata {
  event_metadata owned{};
  owned.run_path = std::move(metadata.run_path);
  owned.agent_name = std::move(metadata.agent_name);
  owned.tool_name = std::move(metadata.tool_name);
  owned.attributes.reserve(metadata.attributes.size());
  for (auto &[key, value] : metadata.attributes) {
    auto owned_value = wh::core::into_owned(std::move(value));
    if (owned_value.has_error()) {
      continue;
    }
    owned.attributes.emplace(std::move(key), std::move(owned_value).value());
  }
  return owned;
}

[[nodiscard]] inline auto make_agent_tool_event(agent_tool_event_record record) -> agent_event {
  if (auto *message = std::get_if<wh::schema::message>(&record.payload); message != nullptr) {
    return make_message_event(std::move(*message), std::move(record.metadata));
  }
  if (auto *custom = std::get_if<agent_tool_custom_event_record>(&record.payload);
      custom != nullptr) {
    return make_custom_event(std::move(custom->name), std::move(custom->payload),
                             std::move(record.metadata));
  }
  auto *error = std::get_if<agent_tool_error_event_record>(&record.payload);
  return make_error_event(error->code, std::move(error->message), std::move(error->detail),
                          std::move(record.metadata));
}

inline auto replay_agent_tool_events(const std::vector<agent_tool_event_record> &records,
                                     wh::adk::detail::live_event_bridge &bridge)
    -> wh::core::result<void> {
  for (const auto &record : records) {
    auto emitted = bridge.emit(make_agent_tool_event(record));
    if (emitted.has_error()) {
      return emitted;
    }
  }
  return {};
}

[[nodiscard]] inline auto make_agent_tool_checkpoint_state(agent_tool_output_summary output)
    -> agent_tool_checkpoint_state {
  return agent_tool_checkpoint_state{
      .events = std::move(output.checkpoint_events),
      .output_chunks = std::move(output.text_chunks),
      .final_message = std::move(output.final_message),
  };
}

[[nodiscard]] inline auto
make_agent_tool_output_summary(const agent_tool_checkpoint_state &checkpoint)
    -> agent_tool_output_summary {
  agent_tool_output_summary output{};
  output.checkpoint_events = checkpoint.events;
  output.text_chunks = checkpoint.output_chunks;
  output.final_message = checkpoint.final_message;
  return output;
}

[[nodiscard]] inline auto
into_owned_agent_tool_child_interrupt(const agent_tool_child_interrupt &interrupt)
    -> wh::core::result<agent_tool_child_interrupt> {
  auto state = wh::core::into_owned(interrupt.state);
  if (state.has_error()) {
    return wh::core::result<agent_tool_child_interrupt>::failure(state.error());
  }
  auto payload = wh::core::into_owned(interrupt.layer_payload);
  if (payload.has_error()) {
    return wh::core::result<agent_tool_child_interrupt>::failure(payload.error());
  }
  return agent_tool_child_interrupt{
      .interrupt_id = interrupt.interrupt_id,
      .location = interrupt.location,
      .state = std::move(state).value(),
      .layer_payload = std::move(payload).value(),
      .parent_locations = interrupt.parent_locations,
      .trigger_reason = interrupt.trigger_reason,
  };
}

[[nodiscard]] inline auto
into_owned_agent_tool_child_interrupt(agent_tool_child_interrupt &&interrupt)
    -> wh::core::result<agent_tool_child_interrupt> {
  auto state = wh::core::into_owned(std::move(interrupt.state));
  if (state.has_error()) {
    return wh::core::result<agent_tool_child_interrupt>::failure(state.error());
  }
  auto payload = wh::core::into_owned(std::move(interrupt.layer_payload));
  if (payload.has_error()) {
    return wh::core::result<agent_tool_child_interrupt>::failure(payload.error());
  }
  return agent_tool_child_interrupt{
      .interrupt_id = std::move(interrupt.interrupt_id),
      .location = std::move(interrupt.location),
      .state = std::move(state).value(),
      .layer_payload = std::move(payload).value(),
      .parent_locations = std::move(interrupt.parent_locations),
      .trigger_reason = std::move(interrupt.trigger_reason),
  };
}

[[nodiscard]] inline auto into_owned_agent_tool_event_record(const agent_tool_event_record &record)
    -> wh::core::result<agent_tool_event_record> {
  auto metadata = capture_bridge_metadata(record.metadata);
  if (const auto *message = std::get_if<wh::schema::message>(&record.payload); message != nullptr) {
    return agent_tool_event_record{
        .payload = *message,
        .metadata = std::move(metadata),
    };
  }
  if (const auto *custom = std::get_if<agent_tool_custom_event_record>(&record.payload);
      custom != nullptr) {
    auto payload = wh::core::into_owned(custom->payload);
    if (payload.has_error()) {
      return wh::core::result<agent_tool_event_record>::failure(payload.error());
    }
    return agent_tool_event_record{
        .payload =
            agent_tool_custom_event_record{
                .name = custom->name,
                .payload = std::move(payload).value(),
            },
        .metadata = std::move(metadata),
    };
  }

  const auto *error = std::get_if<agent_tool_error_event_record>(&record.payload);
  auto detail = wh::core::into_owned(error->detail);
  if (detail.has_error()) {
    return wh::core::result<agent_tool_event_record>::failure(detail.error());
  }
  return agent_tool_event_record{
      .payload =
          agent_tool_error_event_record{
              .code = error->code,
              .message = error->message,
              .detail = std::move(detail).value(),
          },
      .metadata = std::move(metadata),
  };
}

[[nodiscard]] inline auto into_owned_agent_tool_event_record(agent_tool_event_record &&record)
    -> wh::core::result<agent_tool_event_record> {
  auto metadata = capture_bridge_metadata(std::move(record.metadata));
  if (auto *message = std::get_if<wh::schema::message>(&record.payload); message != nullptr) {
    return agent_tool_event_record{
        .payload = std::move(*message),
        .metadata = std::move(metadata),
    };
  }
  if (auto *custom = std::get_if<agent_tool_custom_event_record>(&record.payload);
      custom != nullptr) {
    auto payload = wh::core::into_owned(std::move(custom->payload));
    if (payload.has_error()) {
      return wh::core::result<agent_tool_event_record>::failure(payload.error());
    }
    return agent_tool_event_record{
        .payload =
            agent_tool_custom_event_record{
                .name = std::move(custom->name),
                .payload = std::move(payload).value(),
            },
        .metadata = std::move(metadata),
    };
  }

  auto *error = std::get_if<agent_tool_error_event_record>(&record.payload);
  auto detail = wh::core::into_owned(std::move(error->detail));
  if (detail.has_error()) {
    return wh::core::result<agent_tool_event_record>::failure(detail.error());
  }
  return agent_tool_event_record{
      .payload =
          agent_tool_error_event_record{
              .code = error->code,
              .message = std::move(error->message),
              .detail = std::move(detail).value(),
          },
      .metadata = std::move(metadata),
  };
}

[[nodiscard]] inline auto
into_owned_agent_tool_checkpoint_state(const agent_tool_checkpoint_state &checkpoint)
    -> wh::core::result<agent_tool_checkpoint_state> {
  agent_tool_checkpoint_state owned{};
  owned.events.reserve(checkpoint.events.size());
  for (const auto &record : checkpoint.events) {
    auto owned_record = into_owned_agent_tool_event_record(record);
    if (owned_record.has_error()) {
      return wh::core::result<agent_tool_checkpoint_state>::failure(owned_record.error());
    }
    owned.events.push_back(std::move(owned_record).value());
  }
  owned.output_chunks = checkpoint.output_chunks;
  owned.final_message = checkpoint.final_message;
  return owned;
}

[[nodiscard]] inline auto
into_owned_agent_tool_checkpoint_state(agent_tool_checkpoint_state &&checkpoint)
    -> wh::core::result<agent_tool_checkpoint_state> {
  agent_tool_checkpoint_state owned{};
  owned.events.reserve(checkpoint.events.size());
  for (auto &record : checkpoint.events) {
    auto owned_record = into_owned_agent_tool_event_record(std::move(record));
    if (owned_record.has_error()) {
      return wh::core::result<agent_tool_checkpoint_state>::failure(owned_record.error());
    }
    owned.events.push_back(std::move(owned_record).value());
  }
  owned.output_chunks = std::move(checkpoint.output_chunks);
  owned.final_message = std::move(checkpoint.final_message);
  return owned;
}

[[nodiscard]] inline auto
into_owned_agent_tool_interrupt_state(const agent_tool_interrupt_state &state)
    -> wh::core::result<agent_tool_interrupt_state> {
  auto checkpoint = into_owned_agent_tool_checkpoint_state(state.checkpoint);
  if (checkpoint.has_error()) {
    return wh::core::result<agent_tool_interrupt_state>::failure(checkpoint.error());
  }

  std::optional<agent_tool_child_interrupt> child_interrupt{};
  if (state.child_interrupt.has_value()) {
    auto owned_child = into_owned_agent_tool_child_interrupt(*state.child_interrupt);
    if (owned_child.has_error()) {
      return wh::core::result<agent_tool_interrupt_state>::failure(owned_child.error());
    }
    child_interrupt = std::move(owned_child).value();
  }

  return agent_tool_interrupt_state{
      .checkpoint = std::move(checkpoint).value(),
      .child_interrupt = std::move(child_interrupt),
  };
}

[[nodiscard]] inline auto into_owned_agent_tool_interrupt_state(agent_tool_interrupt_state &&state)
    -> wh::core::result<agent_tool_interrupt_state> {
  auto checkpoint = into_owned_agent_tool_checkpoint_state(std::move(state.checkpoint));
  if (checkpoint.has_error()) {
    return wh::core::result<agent_tool_interrupt_state>::failure(checkpoint.error());
  }

  std::optional<agent_tool_child_interrupt> child_interrupt{};
  if (state.child_interrupt.has_value()) {
    auto owned_child = into_owned_agent_tool_child_interrupt(std::move(*state.child_interrupt));
    if (owned_child.has_error()) {
      return wh::core::result<agent_tool_interrupt_state>::failure(owned_child.error());
    }
    child_interrupt = std::move(owned_child).value();
  }

  return agent_tool_interrupt_state{
      .checkpoint = std::move(checkpoint).value(),
      .child_interrupt = std::move(child_interrupt),
  };
}

[[nodiscard]] inline auto make_child_interrupt_record(const wh::core::interrupt_context &context)
    -> agent_tool_child_interrupt {
  return agent_tool_child_interrupt{
      .interrupt_id = context.interrupt_id,
      .location = context.location,
      .state = capture_bridge_state_any_or_empty(context.state),
      .layer_payload = capture_bridge_state_any_or_empty(context.layer_payload),
      .parent_locations = context.parent_locations,
      .trigger_reason = context.trigger_reason,
  };
}

[[nodiscard]] inline auto to_interrupt_context(const agent_tool_child_interrupt &interrupt)
    -> wh::core::interrupt_context {
  return wh::core::interrupt_context{
      .interrupt_id = interrupt.interrupt_id,
      .location = interrupt.location,
      .state = wh::core::clone_interrupt_payload_any(interrupt.state),
      .layer_payload = wh::core::clone_interrupt_payload_any(interrupt.layer_payload),
      .parent_locations = interrupt.parent_locations,
      .trigger_reason = interrupt.trigger_reason,
  };
}

[[nodiscard]] inline auto find_exact_resume_target_id(const wh::core::resume_state &state,
                                                      const wh::core::address &location)
    -> std::optional<std::string> {
  if (!state.is_exact_resume_target(location)) {
    return std::nullopt;
  }
  const auto ids = state.collect_subtree_interrupt_ids(
      location, wh::core::resume_subtree_query_options{.include_used = true});
  for (const auto &interrupt_id : ids) {
    auto address = state.location_of(interrupt_id);
    if (address.has_value() && address.value().get() == location) {
      return interrupt_id;
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline auto prepare_resume_projection(wh::core::run_context &context,
                                                    const wh::core::address &location)
    -> wh::core::result<std::optional<agent_tool_resume_projection>> {
  if (!context.resume_info.has_value()) {
    return std::optional<agent_tool_resume_projection>{};
  }

  auto outer_interrupt_id = find_exact_resume_target_id(*context.resume_info, location);
  if (!outer_interrupt_id.has_value()) {
    return std::optional<agent_tool_resume_projection>{};
  }
  if (!context.interrupt_info.has_value() || context.interrupt_info->location != location) {
    return wh::core::result<std::optional<agent_tool_resume_projection>>::failure(
        wh::core::errc::not_found);
  }

  const auto *stored =
      wh::core::any_cast<agent_tool_interrupt_state>(&context.interrupt_info->state);
  if (stored == nullptr) {
    return wh::core::result<std::optional<agent_tool_resume_projection>>::failure(
        wh::core::errc::type_mismatch);
  }

  auto patch = wh::compose::consume_resume_data<wh::compose::resume_patch>(*context.resume_info,
                                                                           *outer_interrupt_id);
  if (patch.has_error()) {
    return wh::core::result<std::optional<agent_tool_resume_projection>>::failure(patch.error());
  }

  return std::optional<agent_tool_resume_projection>{agent_tool_resume_projection{
      .outer_interrupt_id = std::move(*outer_interrupt_id),
      .patch = std::move(patch).value(),
      .state = *stored,
  }};
}

inline auto apply_resume_projection(wh::adk::run_request &request,
                                    const agent_tool_resume_projection &projection)
    -> wh::core::result<void> {
  if (!projection.state.child_interrupt.has_value()) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }

  auto child_interrupt = to_interrupt_context(*projection.state.child_interrupt);
  request.options.compose_controls.resume.contexts.push_back(child_interrupt);
  request.options.compose_controls.resume.decision = wh::compose::interrupt_resume_decision{
      .interrupt_context_id = child_interrupt.interrupt_id,
      .decision = projection.patch.decision,
      .edited_payload = projection.patch.data,
      .audit = projection.patch.audit,
  };
  return {};
}

[[nodiscard]] inline auto resolve_child_interrupt(const agent_tool_output_summary &output,
                                                  const wh::core::run_context &context)
    -> std::optional<agent_tool_child_interrupt> {
  if (context.interrupt_info.has_value() && !context.interrupt_info->interrupt_id.empty()) {
    return make_child_interrupt_record(*context.interrupt_info);
  }
  return output.child_interrupt;
}

[[nodiscard]] inline auto resolve_outer_interrupt_id(
    const agent_tool_runtime &runtime, const agent_tool_scope_snapshot &scope,
    const wh::core::run_context &parent,
    const std::optional<agent_tool_resume_projection> &projection,
    const std::optional<wh::core::interrupt_context> &saved_outer_interrupt,
    const std::optional<agent_tool_child_interrupt> &child_interrupt) -> std::string {
  if (projection.has_value() && !projection->outer_interrupt_id.empty()) {
    return projection->outer_interrupt_id;
  }
  if (parent.interrupt_info.has_value() && parent.interrupt_info->location == scope.location &&
      !parent.interrupt_info->interrupt_id.empty()) {
    return parent.interrupt_info->interrupt_id;
  }
  if (saved_outer_interrupt.has_value() && saved_outer_interrupt->location == scope.location &&
      !saved_outer_interrupt->interrupt_id.empty()) {
    return saved_outer_interrupt->interrupt_id;
  }

  std::string interrupt_id{agent_tool_interrupt_id_prefix};
  interrupt_id.append(runtime.tool_name);
  interrupt_id.push_back(':');
  interrupt_id.append(scope.call_id);
  interrupt_id.push_back(':');
  if (child_interrupt.has_value() && !child_interrupt->interrupt_id.empty()) {
    interrupt_id.append(child_interrupt->interrupt_id);
  } else {
    interrupt_id.append(agent_tool_interrupt_default_suffix);
  }
  return interrupt_id;
}

inline auto
project_child_runtime(wh::core::run_context &parent, const agent_tool_scope_snapshot &scope,
                      const std::optional<wh::core::interrupt_context> &saved_outer_interrupt,
                      const agent_tool_runtime &runtime,
                      const std::optional<agent_tool_resume_projection> &projection,
                      const agent_tool_output_summary &output) -> void {
  if (!output.interrupted) {
    if (parent.interrupt_info.has_value() && parent.interrupt_info->location == scope.location) {
      parent.interrupt_info.reset();
    }
    return;
  }

  auto child_interrupt = resolve_child_interrupt(output, parent);
  auto outer_interrupt_id = resolve_outer_interrupt_id(runtime, scope, parent, projection,
                                                       saved_outer_interrupt, child_interrupt);

  parent.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = std::move(outer_interrupt_id),
      .location = scope.location,
      .state = wh::core::any(agent_tool_interrupt_state{
          .checkpoint = make_agent_tool_checkpoint_state(agent_tool_output_summary{
              .checkpoint_events = output.checkpoint_events,
              .text_chunks = output.text_chunks,
              .final_message = output.final_message,
              .child_interrupt = output.child_interrupt,
              .interrupted = output.interrupted,
          }),
          .child_interrupt = child_interrupt,
      }),
      .trigger_reason = child_interrupt.has_value() ? child_interrupt->trigger_reason
                                                    : std::string{agent_tool_interrupt_reason},
  };
  if (child_interrupt.has_value()) {
    parent.interrupt_info->layer_payload =
        wh::core::clone_interrupt_payload_any(child_interrupt->layer_payload);
  }
}

[[nodiscard]] inline auto parse_request_text(const std::string_view input_json)
    -> wh::core::result<std::string> {
  auto parsed = wh::core::parse_json(input_json);
  if (parsed.has_error()) {
    return wh::core::result<std::string>::failure(parsed.error());
  }
  if (!parsed.value().IsObject()) {
    return wh::core::result<std::string>::failure(wh::core::errc::type_mismatch);
  }

  auto request_value = wh::core::json_find_member(parsed.value(), agent_tool_request_json_key);
  if (request_value.has_error()) {
    return wh::core::result<std::string>::failure(request_value.error());
  }
  if (!request_value.value()->IsString()) {
    return wh::core::result<std::string>::failure(wh::core::errc::type_mismatch);
  }
  return std::string{
      request_value.value()->GetString(),
      static_cast<std::size_t>(request_value.value()->GetStringLength()),
  };
}

[[nodiscard]] inline auto
build_agent_tool_request(const agent_tool_runtime &runtime, const wh::compose::tool_call &call,
                         [[maybe_unused]] const wh::core::run_context &context)
    -> wh::core::result<wh::adk::run_request> {
  switch (runtime.input_mode) {
  case agent_tool_input_mode::request: {
    auto request_text = parse_request_text(call.arguments);
    if (request_text.has_error()) {
      return wh::core::result<wh::adk::run_request>::failure(request_text.error());
    }
    wh::adk::run_request request{};
    request.messages.push_back(make_user_message(std::move(request_text).value()));
    return request;
  }
  case agent_tool_input_mode::message_history: {
    auto history_payload = wh::adk::detail::read_history_request_payload_view(call.payload);
    if (history_payload.has_error()) {
      return wh::core::result<wh::adk::run_request>::failure(history_payload.error());
    }
    return wh::adk::run_request{
        .messages = history_payload.value().history_request->messages,
    };
  }
  case agent_tool_input_mode::custom_schema: {
    wh::adk::run_request request{};
    request.messages.push_back(make_user_message(call.arguments));
    return request;
  }
  }

  return wh::core::result<wh::adk::run_request>::failure(wh::core::errc::not_supported);
}

[[nodiscard]] inline auto
prepare_agent_tool_run_setup(const agent_tool_runtime &runtime, const wh::compose::tool_call &call,
                             wh::core::run_context &context, const agent_tool_scope_snapshot &scope)
    -> wh::core::result<agent_tool_run_setup> {
  auto request = build_agent_tool_request(runtime, call, context);
  if (request.has_error()) {
    return wh::core::result<agent_tool_run_setup>::failure(request.error());
  }

  auto projection = prepare_resume_projection(context, scope.location);
  if (projection.has_error()) {
    return wh::core::result<agent_tool_run_setup>::failure(projection.error());
  }
  if (projection.value().has_value()) {
    auto lowered = apply_resume_projection(request.value(), *projection.value());
    if (lowered.has_error()) {
      return wh::core::result<agent_tool_run_setup>::failure(lowered.error());
    }
  }

  const auto cleared_outer_interrupt =
      context.interrupt_info.has_value() && context.interrupt_info->location == scope.location;
  auto saved_outer_interrupt = cleared_outer_interrupt
                                   ? context.interrupt_info
                                   : std::optional<wh::core::interrupt_context>{};
  if (cleared_outer_interrupt) {
    context.interrupt_info.reset();
  }

  return agent_tool_run_setup{
      .request = std::move(request).value(),
      .projection = std::move(projection).value(),
      .saved_outer_interrupt = std::move(saved_outer_interrupt),
      .cleared_outer_interrupt = cleared_outer_interrupt,
  };
}

inline auto restore_outer_interrupt(wh::core::run_context &context,
                                    const agent_tool_run_setup &setup) -> void {
  if (!setup.cleared_outer_interrupt || context.interrupt_info.has_value()) {
    return;
  }
  context.interrupt_info = setup.saved_outer_interrupt;
}

[[nodiscard]] inline auto materialize_agent_tool_output(const agent_tool_runtime &runtime,
                                                        agent_run_output artifact,
                                                        const agent_tool_scope_snapshot &scope,
                                                        wh::adk::detail::live_event_bridge &bridge,
                                                        agent_tool_output_summary &output)
    -> wh::core::result<void> {
  bool emitted_boundary_event = false;
  while (true) {
    auto next = read_agent_event_stream(artifact.events);
    if (next.has_error()) {
      return wh::core::result<void>::failure(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.error.failed()) {
      return wh::core::result<void>::failure(chunk.error);
    }
    if (chunk.eof) {
      break;
    }
    if (!chunk.value.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::protocol_error);
    }

    auto event = std::move(*chunk.value);
    auto normalized_metadata = normalize_child_metadata(runtime, scope, std::move(event.metadata));

    if (auto *message = std::get_if<message_event>(&event.payload); message != nullptr) {
      auto consumed = consume_message_event_messages(
          std::move(*message), [&](wh::schema::message entry) -> wh::core::result<void> {
            auto text = render_message_text(entry);
            if (!text.empty()) {
              output.text_chunks.push_back(std::move(text));
            }
            output.final_message = entry;
            if (!runtime.forward_internal_events) {
              return {};
            }

            auto checkpoint_metadata = capture_bridge_metadata(normalized_metadata);
            output.checkpoint_events.push_back(agent_tool_event_record{
                .payload = entry,
                .metadata = std::move(checkpoint_metadata),
            });
            emitted_boundary_event = true;
            return bridge.emit(make_message_event(std::move(entry), normalized_metadata));
          });
      if (consumed.has_error()) {
        return consumed;
      }
      continue;
    }

    if (const auto *action = std::get_if<control_action>(&event.payload); action != nullptr) {
      if (action->kind == control_action_kind::interrupt) {
        output.interrupted = true;
        output.child_interrupt = agent_tool_child_interrupt{
            .interrupt_id = action->interrupt_id,
            .location = normalized_metadata.run_path,
        };
        emitted_boundary_event = true;
        auto emitted = bridge.emit(make_control_event(*action, std::move(normalized_metadata)));
        if (emitted.has_error()) {
          return emitted;
        }
      }
      continue;
    }

    if (const auto *error = std::get_if<error_event>(&event.payload); error != nullptr) {
      output.final_error = error->code;
      auto checkpoint_detail = capture_bridge_state_any_or_empty(error->detail);
      auto checkpoint_metadata = capture_bridge_metadata(normalized_metadata);
      output.checkpoint_events.push_back(agent_tool_event_record{
          .payload =
              agent_tool_error_event_record{
                  .code = error->code,
                  .message = error->message,
                  .detail = std::move(checkpoint_detail),
              },
          .metadata = std::move(checkpoint_metadata),
      });
      emitted_boundary_event = true;
      auto emitted = bridge.emit(make_error_event(error->code, error->message, error->detail,
                                                  std::move(normalized_metadata)));
      if (emitted.has_error()) {
        return emitted;
      }
      continue;
    }

    if (runtime.forward_internal_events) {
      if (const auto *custom = std::get_if<custom_event>(&event.payload); custom != nullptr) {
        auto checkpoint_payload = capture_bridge_state_any_or_empty(custom->payload);
        auto checkpoint_metadata = capture_bridge_metadata(normalized_metadata);
        output.checkpoint_events.push_back(agent_tool_event_record{
            .payload =
                agent_tool_custom_event_record{
                    .name = custom->name,
                    .payload = std::move(checkpoint_payload),
                },
            .metadata = std::move(checkpoint_metadata),
        });
        emitted_boundary_event = true;
        auto emitted = bridge.emit(
            make_custom_event(custom->name, custom->payload, std::move(normalized_metadata)));
        if (emitted.has_error()) {
          return emitted;
        }
      }
    }
  }

  if (!output.final_message.has_value() && artifact.final_message.has_value()) {
    output.final_message = std::move(artifact.final_message);
    auto text = render_message_text(*output.final_message);
    if (!text.empty()) {
      output.text_chunks.push_back(std::move(text));
    }
  }

  if (!runtime.forward_internal_events && output.final_message.has_value()) {
    auto checkpoint_metadata = capture_bridge_metadata(default_tool_metadata(runtime, scope));
    output.checkpoint_events.push_back(agent_tool_event_record{
        .payload = *output.final_message,
        .metadata = std::move(checkpoint_metadata),
    });
    emitted_boundary_event = true;
    auto emitted = bridge.emit(
        make_message_event(*output.final_message, default_tool_metadata(runtime, scope)));
    if (emitted.has_error()) {
      return emitted;
    }
  }

  if (!emitted_boundary_event && output.text_chunks.empty() && !output.final_error.has_value() &&
      !output.interrupted) {
    return wh::core::result<void>::failure(wh::core::errc::protocol_error);
  }

  return {};
}

[[nodiscard]] inline auto run_agent_tool(const agent_tool_runtime &runtime,
                                         const wh::compose::tool_call &call,
                                         const wh::tool::call_scope &scope)
    -> wh::core::result<agent_tool_result> {
  const auto scope_snapshot = make_agent_tool_scope_snapshot(scope);
  auto setup = prepare_agent_tool_run_setup(runtime, call, scope.run, scope_snapshot);
  if (setup.has_error()) {
    return wh::core::result<agent_tool_result>::failure(setup.error());
  }

  auto bridge = wh::adk::detail::make_live_event_bridge();
  agent_tool_output_summary output{};
  if (setup.value().projection.has_value()) {
    output = make_agent_tool_output_summary(setup.value().projection->state.checkpoint);
    auto replayed = replay_agent_tool_events(output.checkpoint_events, bridge);
    if (replayed.has_error()) {
      return wh::core::result<agent_tool_result>::failure(replayed.error());
    }
  }

  auto run_result = runtime.runner(setup.value().request, scope.run);
  if (run_result.has_error()) {
    restore_outer_interrupt(scope.run, setup.value());
    output.final_error = run_result.error();
    auto emitted = bridge.emit(make_error_event(run_result.error(),
                                                std::string{agent_tool_bridge_failed_message}, {},
                                                default_tool_metadata(runtime, scope_snapshot)));
    if (emitted.has_error()) {
      return wh::core::result<agent_tool_result>::failure(emitted.error());
    }
    auto closed = bridge.close();
    if (closed.has_error()) {
      return wh::core::result<agent_tool_result>::failure(closed.error());
    }
    return agent_tool_result{
        .events = std::move(bridge).release_reader(),
        .final_error = run_result.error(),
    };
  }

  auto materialized = materialize_agent_tool_output(runtime, std::move(run_result).value(),
                                                    scope_snapshot, bridge, output);
  if (materialized.has_error()) {
    restore_outer_interrupt(scope.run, setup.value());
    return wh::core::result<agent_tool_result>::failure(materialized.error());
  }

  project_child_runtime(scope.run, scope_snapshot, setup.value().saved_outer_interrupt, runtime,
                        setup.value().projection, output);

  std::string joined_text{};
  for (const auto &chunk : output.text_chunks) {
    joined_text.append(chunk);
  }

  auto closed = bridge.close();
  if (closed.has_error()) {
    return wh::core::result<agent_tool_result>::failure(closed.error());
  }

  return agent_tool_result{
      .events = std::move(bridge).release_reader(),
      .output_chunks = std::move(output.text_chunks),
      .output_text = std::move(joined_text),
      .final_message = std::move(output.final_message),
      .final_error = output.final_error,
      .interrupted = output.interrupted,
  };
}

class agent_tool_live_stream_reader final
    : public wh::schema::stream::stream_base<agent_tool_live_stream_reader,
                                             wh::compose::graph_value> {
public:
  using value_type = wh::compose::graph_value;
  using chunk_type = wh::schema::stream::stream_chunk<value_type>;
  using result_type = wh::schema::stream::stream_result<chunk_type>;
  using try_result_type = wh::schema::stream::stream_try_result<chunk_type>;
  using event_result_t = wh::adk::agent_event_stream_result;
  using message_result_t =
      wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<wh::schema::message>>;

  agent_tool_live_stream_reader() = default;
  agent_tool_live_stream_reader(const agent_tool_live_stream_reader &) = delete;
  auto operator=(const agent_tool_live_stream_reader &) -> agent_tool_live_stream_reader & = delete;
  agent_tool_live_stream_reader(agent_tool_live_stream_reader &&) noexcept = default;
  auto operator=(agent_tool_live_stream_reader &&) noexcept
      -> agent_tool_live_stream_reader & = default;

  agent_tool_live_stream_reader(agent_tool_runtime runtime, agent_tool_scope_snapshot scope,
                                wh::core::run_context &context, agent_tool_run_setup setup,
                                agent_run_output artifact)
      : runtime_(std::move(runtime)), scope_(std::move(scope)), context_(&context),
        projection_(std::move(setup.projection)),
        saved_outer_interrupt_(std::move(setup.saved_outer_interrupt)),
        live_events_(std::move(artifact.events)),
        final_message_fallback_(std::move(artifact.final_message)),
        cleared_outer_interrupt_(setup.cleared_outer_interrupt) {
    if (projection_.has_value()) {
      output_ = make_agent_tool_output_summary(projection_->state.checkpoint);
      replay_chunks_ = output_.text_chunks;
    }
    boundary_event_seen_ = !replay_chunks_.empty();
  }

  ~agent_tool_live_stream_reader() { cleanup_for_drop(); }

  [[nodiscard]] auto read_impl() -> result_type {
    while (true) {
      if (auto replay = poll_replay_chunk(); replay.has_value()) {
        return std::move(*replay);
      }
      if (closed_) {
        return result_type{chunk_type::make_eof()};
      }

      if (active_message_reader_.has_value()) {
        auto mapped = process_active_message_result(active_message_reader_->read());
        if (mapped.has_value()) {
          return std::move(*mapped);
        }
        continue;
      }

      auto mapped = process_event_result(read_agent_event_stream(live_events_));
      if (mapped.has_value()) {
        return std::move(*mapped);
      }
    }
  }

  [[nodiscard]] auto try_read_impl() -> try_result_type {
    while (true) {
      if (auto replay = poll_replay_chunk(); replay.has_value()) {
        return std::move(*replay);
      }
      if (closed_) {
        return result_type{chunk_type::make_eof()};
      }

      if (active_message_reader_.has_value()) {
        auto next = active_message_reader_->try_read();
        if (std::holds_alternative<wh::schema::stream::stream_signal>(next)) {
          return wh::schema::stream::stream_pending;
        }
        auto mapped = process_active_message_result(std::move(std::get<message_result_t>(next)));
        if (mapped.has_value()) {
          return std::move(*mapped);
        }
        continue;
      }

      auto next = live_events_.try_read();
      if (std::holds_alternative<wh::schema::stream::stream_signal>(next)) {
        return wh::schema::stream::stream_pending;
      }
      auto mapped = process_event_result(std::move(std::get<event_result_t>(next)));
      if (mapped.has_value()) {
        return std::move(*mapped);
      }
    }
  }

  class read_sender {
  public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(result_type),
                                       stdexec::set_error_t(std::exception_ptr),
                                       stdexec::set_stopped_t()>;

    explicit read_sender(agent_tool_live_stream_reader &owner) noexcept : owner_(&owner) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    class operation : public wh::core::detail::inline_drive_loop<operation<receiver_t>> {
      using drive_loop_t = wh::core::detail::inline_drive_loop<operation<receiver_t>>;
      friend drive_loop_t;
      friend class wh::core::detail::callback_guard<operation>;
      using receiver_env_t =
          std::remove_cvref_t<decltype(stdexec::get_env(std::declval<const receiver_t &>()))>;
      using final_completion_t = wh::core::detail::receiver_completion<receiver_t, result_type>;

      struct stopped_tag {};
      using child_completion_t =
          std::variant<event_result_t, message_result_t, std::exception_ptr, stopped_tag>;

      struct child_receiver {
        using receiver_concept = stdexec::receiver_t;

        operation *self{nullptr};
        receiver_env_t env_{};

        auto set_value(event_result_t status) && noexcept -> void {
          auto scope = self->callbacks_.enter(self);
          self->finish_child(child_completion_t{std::move(status)});
        }

        auto set_value(message_result_t status) && noexcept -> void {
          auto scope = self->callbacks_.enter(self);
          self->finish_child(child_completion_t{std::move(status)});
        }

        template <typename error_t> auto set_error(error_t &&error) && noexcept -> void {
          auto scope = self->callbacks_.enter(self);
          if constexpr (std::same_as<std::remove_cvref_t<error_t>, std::exception_ptr>) {
            self->finish_child(child_completion_t{std::forward<error_t>(error)});
          } else {
            try {
              throw std::forward<error_t>(error);
            } catch (...) {
              self->finish_child(child_completion_t{std::current_exception()});
            }
          }
        }

        auto set_stopped() && noexcept -> void {
          auto scope = self->callbacks_.enter(self);
          self->finish_child(child_completion_t{stopped_tag{}});
        }

        [[nodiscard]] auto get_env() const noexcept -> receiver_env_t { return env_; }
      };

      using event_child_sender_t =
          decltype(std::declval<wh::adk::agent_event_stream_reader &>().read_async());
      using message_child_sender_t =
          decltype(std::declval<wh::adk::agent_message_stream_reader &>().read_async());
      using event_child_op_t = stdexec::connect_result_t<event_child_sender_t, child_receiver>;
      using message_child_op_t = stdexec::connect_result_t<message_child_sender_t, child_receiver>;

      enum class child_kind : std::uint8_t { none = 0U, event, message };

    public:
      using operation_state_concept = stdexec::operation_state_t;

      operation(agent_tool_live_stream_reader *owner, receiver_t receiver)
          : owner_(owner), receiver_(std::move(receiver)), env_(stdexec::get_env(receiver_)) {}

      auto start() & noexcept -> void { request_drive(); }

    private:
      [[nodiscard]] auto finished() const noexcept -> bool {
        return delivered_.load(std::memory_order_acquire);
      }

      [[nodiscard]] auto completion_pending() const noexcept -> bool {
        return pending_completion_.has_value();
      }

      [[nodiscard]] auto take_completion() noexcept -> std::optional<final_completion_t> {
        if (!pending_completion_.has_value()) {
          return std::nullopt;
        }
        auto completion = std::move(pending_completion_);
        pending_completion_.reset();
        return completion;
      }

      auto on_callback_exit() noexcept -> void {
        if (completion_.ready()) {
          request_drive();
        }
      }

      auto request_drive() noexcept -> void { drive_loop_t::request_drive(); }

      auto drive() noexcept -> void {
        while (!finished()) {
          if (callbacks_.active()) {
            return;
          }

          if (auto current = completion_.take(); current.has_value()) {
            reset_child();
            if (auto *event = std::get_if<event_result_t>(&*current); event != nullptr) {
              auto mapped = owner_->process_event_result(std::move(*event));
              if (mapped.has_value()) {
                complete_value(std::move(*mapped));
                return;
              }
              continue;
            }
            if (auto *message = std::get_if<message_result_t>(&*current); message != nullptr) {
              auto mapped = owner_->process_active_message_result(std::move(*message));
              if (mapped.has_value()) {
                complete_value(std::move(*mapped));
                return;
              }
              continue;
            }
            if (auto *error = std::get_if<std::exception_ptr>(&*current); error != nullptr) {
              complete_error(std::move(*error));
              return;
            }
            complete_stopped();
            return;
          }

          if (auto replay = owner_->poll_replay_chunk(); replay.has_value()) {
            complete_value(std::move(*replay));
            return;
          }
          if (owner_->closed_) {
            complete_value(result_type{chunk_type::make_eof()});
            return;
          }

          if (owner_->active_message_reader_.has_value()) {
            start_message_child();
            return;
          }

          start_event_child();
          return;
        }
      }

      auto reset_child() noexcept -> void {
        switch (child_kind_) {
        case child_kind::none:
          return;
        case child_kind::event:
          event_child_op_.reset();
          break;
        case child_kind::message:
          message_child_op_.reset();
          break;
        }
        child_kind_ = child_kind::none;
      }

      auto start_event_child() noexcept -> void {
        try {
          event_child_op_.emplace_from(stdexec::connect, owner_->live_events_.read_async(),
                                       child_receiver{this, env_});
          child_kind_ = child_kind::event;
          stdexec::start(event_child_op_.get());
        } catch (...) {
          complete_error(std::current_exception());
        }
      }

      auto start_message_child() noexcept -> void {
        if (!owner_->active_message_reader_.has_value()) {
          request_drive();
          return;
        }

        try {
          message_child_op_.emplace_from(stdexec::connect,
                                         owner_->active_message_reader_->read_async(),
                                         child_receiver{this, env_});
          child_kind_ = child_kind::message;
          stdexec::start(message_child_op_.get());
        } catch (...) {
          complete_error(std::current_exception());
        }
      }

      auto finish_child(child_completion_t completion) noexcept -> void {
        if (finished()) {
          return;
        }
        if (!completion_.publish(std::move(completion))) {
          std::terminate();
        }
        request_drive();
      }

      auto complete_value(result_type status) noexcept -> void {
        if (delivered_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        pending_completion_.emplace(
            final_completion_t::set_value(std::move(receiver_), std::move(status)));
      }

      auto complete_error(std::exception_ptr error) noexcept -> void {
        if (delivered_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        pending_completion_.emplace(
            final_completion_t::set_error(std::move(receiver_), std::move(error)));
      }

      auto complete_stopped() noexcept -> void {
        if (delivered_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        pending_completion_.emplace(final_completion_t::set_stopped(std::move(receiver_)));
      }

      agent_tool_live_stream_reader *owner_{nullptr};
      receiver_t receiver_;
      receiver_env_t env_{};
      wh::core::detail::manual_lifetime_box<event_child_op_t> event_child_op_{};
      wh::core::detail::manual_lifetime_box<message_child_op_t> message_child_op_{};
      wh::core::detail::single_completion_slot<child_completion_t> completion_{};
      wh::core::detail::callback_guard<operation> callbacks_{};
      std::optional<final_completion_t> pending_completion_{};
      std::atomic<bool> delivered_{false};
      child_kind child_kind_{child_kind::none};
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) && -> operation<receiver_t> {
      return operation<receiver_t>{owner_, std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> wh::core::detail::async_completion_env {
      return {};
    }

  private:
    agent_tool_live_stream_reader *owner_{nullptr};
  };

  [[nodiscard]] auto read_async() & -> read_sender { return read_sender{*this}; }

  auto close_impl() -> wh::core::result<void> {
    closed_ = true;
    auto closed = close_sources();
    finalize_without_interrupt();
    return closed;
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool { return closed_; }

private:
  [[nodiscard]] auto make_value_result(std::string text) -> result_type {
    return result_type{chunk_type::make_value(wh::compose::graph_value{std::move(text)})};
  }

  [[nodiscard]] auto poll_replay_chunk() -> std::optional<result_type> {
    if (replay_index_ >= replay_chunks_.size()) {
      return std::nullopt;
    }
    return make_value_result(replay_chunks_[replay_index_++]);
  }

  auto finalize_without_interrupt() -> void {
    if (finalized_) {
      return;
    }
    finalized_ = true;
    if (context_ == nullptr) {
      return;
    }
    if (context_->interrupt_info.has_value() &&
        context_->interrupt_info->location == scope_.location) {
      context_->interrupt_info.reset();
    }
    restore_outer_interrupt(*context_, agent_tool_run_setup{
                                           .projection = projection_,
                                           .saved_outer_interrupt = saved_outer_interrupt_,
                                           .cleared_outer_interrupt = cleared_outer_interrupt_,
                                       });
  }

  auto cleanup_for_drop() noexcept -> void {
    closed_ = true;
    [[maybe_unused]] const auto closed = close_sources();
    if (!output_.interrupted) {
      finalize_without_interrupt();
    }
  }

  auto close_sources() -> wh::core::result<void> {
    if (active_message_reader_.has_value()) {
      auto closed = active_message_reader_->close();
      active_message_reader_.reset();
      if (closed.has_error() && closed.error() != wh::core::errc::not_found) {
        return closed;
      }
    }

    auto closed = close_agent_event_stream(live_events_);
    if (closed.has_error() && closed.error() != wh::core::errc::not_found) {
      return closed;
    }
    return {};
  }

  [[nodiscard]] auto finish_error(wh::core::error_code error) -> result_type {
    output_.final_error = error;
    closed_ = true;
    [[maybe_unused]] const auto closed = close_sources();
    finalize_without_interrupt();
    return result_type{chunk_type{.error = error}};
  }

  [[nodiscard]] auto capture_child_interrupt(const control_action &action,
                                             const event_metadata &metadata) const
      -> agent_tool_child_interrupt {
    if (context_ != nullptr && context_->interrupt_info.has_value() &&
        !context_->interrupt_info->interrupt_id.empty()) {
      return make_child_interrupt_record(*context_->interrupt_info);
    }
    return agent_tool_child_interrupt{
        .interrupt_id = action.interrupt_id,
        .location = metadata.run_path,
        .trigger_reason =
            action.reason.empty() ? std::string{agent_tool_interrupt_reason} : action.reason,
    };
  }

  [[nodiscard]] auto finish_interrupt(const control_action &action, const event_metadata &metadata)
      -> result_type {
    output_.interrupted = true;
    output_.child_interrupt = capture_child_interrupt(action, metadata);
    project_child_runtime(*context_, scope_, saved_outer_interrupt_, runtime_, projection_,
                          output_);
    closed_ = true;
    [[maybe_unused]] const auto closed = close_sources();
    finalized_ = true;
    return result_type{chunk_type{.error = wh::core::make_error(wh::core::errc::canceled)}};
  }

  [[nodiscard]] auto maybe_emit_final_message() -> std::optional<result_type> {
    if (live_message_seen_ || !final_message_fallback_.has_value()) {
      return std::nullopt;
    }

    boundary_event_seen_ = true;
    live_message_seen_ = true;
    output_.final_message = *final_message_fallback_;
    auto text = render_message_text(*final_message_fallback_);
    final_message_fallback_.reset();
    if (text.empty()) {
      return std::nullopt;
    }
    output_.text_chunks.push_back(text);
    return make_value_result(std::move(text));
  }

  [[nodiscard]] auto process_message(wh::schema::message message) -> std::optional<result_type> {
    boundary_event_seen_ = true;
    live_message_seen_ = true;
    output_.final_message = message;
    auto text = render_message_text(message);
    if (text.empty()) {
      return std::nullopt;
    }
    output_.text_chunks.push_back(text);
    return make_value_result(std::move(text));
  }

  [[nodiscard]] auto process_active_message_result(message_result_t next)
      -> std::optional<result_type> {
    if (next.has_error() && next.error() == wh::core::errc::not_found) {
      active_message_reader_.reset();
      return std::nullopt;
    }
    if (next.has_error()) {
      active_message_reader_.reset();
      return finish_error(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.error.failed()) {
      active_message_reader_.reset();
      return finish_error(chunk.error);
    }
    if (chunk.eof) {
      active_message_reader_.reset();
      return std::nullopt;
    }
    if (!chunk.value.has_value()) {
      active_message_reader_.reset();
      return finish_error(wh::core::errc::protocol_error);
    }
    return process_message(std::move(*chunk.value));
  }

  [[nodiscard]] auto process_event_result(event_result_t next) -> std::optional<result_type> {
    if (next.has_error() && next.error() == wh::core::errc::not_found) {
      if (auto final_message = maybe_emit_final_message(); final_message.has_value()) {
        return final_message;
      }
      closed_ = true;
      [[maybe_unused]] const auto closed = close_sources();
      finalize_without_interrupt();
      if (!boundary_event_seen_) {
        return finish_error(wh::core::errc::protocol_error);
      }
      return result_type{chunk_type::make_eof()};
    }
    if (next.has_error()) {
      return finish_error(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.eof) {
      if (auto final_message = maybe_emit_final_message(); final_message.has_value()) {
        return final_message;
      }
      closed_ = true;
      [[maybe_unused]] const auto closed = close_sources();
      finalize_without_interrupt();
      if (!boundary_event_seen_) {
        return finish_error(wh::core::errc::protocol_error);
      }
      return result_type{chunk_type::make_eof()};
    }
    if (chunk.error.failed()) {
      return finish_error(chunk.error);
    }
    if (!chunk.value.has_value()) {
      return std::nullopt;
    }

    auto event = std::move(*chunk.value);
    auto normalized_metadata =
        normalize_child_metadata(runtime_, scope_, std::move(event.metadata));

    if (auto *message = std::get_if<message_event>(&event.payload); message != nullptr) {
      boundary_event_seen_ = true;
      if (auto *value = std::get_if<wh::schema::message>(&message->content); value != nullptr) {
        return process_message(std::move(*value));
      }
      auto *stream = std::get_if<agent_message_stream_reader>(&message->content);
      if (stream == nullptr) {
        return finish_error(wh::core::errc::type_mismatch);
      }
      active_message_reader_.emplace(std::move(*stream));
      return std::nullopt;
    }

    if (const auto *action = std::get_if<control_action>(&event.payload); action != nullptr) {
      if (action->kind == control_action_kind::interrupt) {
        boundary_event_seen_ = true;
        return finish_interrupt(*action, normalized_metadata);
      }
      return std::nullopt;
    }

    if (const auto *error = std::get_if<error_event>(&event.payload); error != nullptr) {
      boundary_event_seen_ = true;
      return finish_error(error->code);
    }

    return std::nullopt;
  }

  agent_tool_runtime runtime_{};
  agent_tool_scope_snapshot scope_{};
  wh::core::run_context *context_{nullptr};
  std::optional<agent_tool_resume_projection> projection_{};
  std::optional<wh::core::interrupt_context> saved_outer_interrupt_{};
  wh::adk::agent_event_stream_reader live_events_{};
  std::optional<wh::adk::agent_message_stream_reader> active_message_reader_{};
  std::vector<std::string> replay_chunks_{};
  std::optional<wh::schema::message> final_message_fallback_{};
  agent_tool_output_summary output_{};
  std::size_t replay_index_{0U};
  bool cleared_outer_interrupt_{false};
  bool boundary_event_seen_{false};
  bool live_message_seen_{false};
  bool closed_{false};
  bool finalized_{false};
};

[[nodiscard]] inline auto open_agent_tool_stream(const agent_tool_runtime &runtime,
                                                 const wh::compose::tool_call &call,
                                                 const wh::tool::call_scope &scope)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  auto scope_snapshot = make_agent_tool_scope_snapshot(scope);
  auto setup = prepare_agent_tool_run_setup(runtime, call, scope.run, scope_snapshot);
  if (setup.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(setup.error());
  }

  auto run_result = runtime.runner(setup.value().request, scope.run);
  if (run_result.has_error()) {
    restore_outer_interrupt(scope.run, setup.value());
    return wh::core::result<wh::compose::graph_stream_reader>::failure(run_result.error());
  }

  return wh::compose::graph_stream_reader{
      agent_tool_live_stream_reader{runtime, std::move(scope_snapshot), scope.run,
                                    std::move(setup).value(), std::move(run_result).value()}};
}

[[nodiscard]] inline auto default_message_history_schema() -> std::string {
  return std::string{agent_tool_history_json_schema};
}

[[nodiscard]] inline auto materialize_agent_tool_value(agent_tool_result result)
    -> wh::core::result<wh::compose::graph_value> {
  if (result.final_error.has_value()) {
    return wh::core::result<wh::compose::graph_value>::failure(*result.final_error);
  }
  if (result.interrupted) {
    return wh::core::result<wh::compose::graph_value>::failure(wh::core::errc::canceled);
  }
  if (result.output_text.empty()) {
    return wh::core::result<wh::compose::graph_value>::failure(wh::core::errc::protocol_error);
  }
  return wh::compose::graph_value{std::move(result.output_text)};
}

} // namespace wh::adk::detail

namespace wh::adk {

inline auto agent_tool::tool_schema() const -> wh::schema::tool_schema_definition {
  wh::schema::tool_schema_definition schema{};
  schema.name = name_;
  schema.description = description_;
  switch (input_mode_) {
  case agent_tool_input_mode::request:
    schema.parameters.push_back(wh::schema::tool_parameter_schema{
        .name = std::string{detail::agent_tool_request_json_key},
        .type = wh::schema::tool_parameter_type::string,
        .description = "tool request text",
        .required = true,
    });
    return schema;
  case agent_tool_input_mode::message_history:
    schema.raw_parameters_json_schema = detail::default_message_history_schema();
    return schema;
  case agent_tool_input_mode::custom_schema:
    if (custom_schema_.has_value()) {
      schema.parameters = custom_schema_->parameters;
      schema.raw_parameters_json_schema = custom_schema_->raw_parameters_json_schema;
    }
    return schema;
  }
  return schema;
}

inline auto agent_tool::run(const wh::compose::tool_call &call,
                            const wh::tool::call_scope &scope) const
    -> wh::core::result<agent_tool_result> {
  auto *mutable_self = const_cast<agent_tool *>(this);
  auto runtime = detail::agent_tool_access::make_runtime(*mutable_self);
  if (runtime.has_error()) {
    return wh::core::result<agent_tool_result>::failure(runtime.error());
  }
  return detail::run_agent_tool(runtime.value(), call, scope);
}

inline auto agent_tool::stream(const wh::compose::tool_call &call,
                               const wh::tool::call_scope &scope) const
    -> wh::core::result<wh::compose::graph_stream_reader> {
  auto *mutable_self = const_cast<agent_tool *>(this);
  auto runtime = detail::agent_tool_access::make_runtime(*mutable_self);
  if (runtime.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(runtime.error());
  }
  return detail::open_agent_tool_stream(runtime.value(), call, scope);
}

inline auto agent_tool::compose_entry() const -> wh::core::result<wh::compose::tool_entry> {
  auto *mutable_self = const_cast<agent_tool *>(this);
  auto runtime = detail::agent_tool_access::make_runtime(*mutable_self);
  if (runtime.has_error()) {
    return wh::core::result<wh::compose::tool_entry>::failure(runtime.error());
  }

  wh::compose::tool_entry entry{};
  entry.invoke = wh::compose::tool_invoke{
      [runtime = runtime.value()](const wh::compose::tool_call &call, wh::tool::call_scope scope)
          -> wh::core::result<wh::compose::graph_value> {
        auto result = detail::run_agent_tool(runtime, call, scope);
        if (result.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(result.error());
        }
        return detail::materialize_agent_tool_value(std::move(result).value());
      }};
  entry.stream = wh::compose::tool_stream{
      [runtime = runtime.value()](const wh::compose::tool_call &call, wh::tool::call_scope scope)
          -> wh::core::result<wh::compose::graph_stream_reader> {
        return detail::open_agent_tool_stream(runtime, call, scope);
      }};
  return entry;
}

} // namespace wh::adk

namespace wh::core {

template <> struct any_owned_traits<wh::adk::detail::agent_tool_interrupt_state> {
  [[nodiscard]] static auto into_owned(const wh::adk::detail::agent_tool_interrupt_state &value)
      -> wh::core::result<wh::adk::detail::agent_tool_interrupt_state> {
    return wh::adk::detail::into_owned_agent_tool_interrupt_state(value);
  }

  [[nodiscard]] static auto into_owned(wh::adk::detail::agent_tool_interrupt_state &&value)
      -> wh::core::result<wh::adk::detail::agent_tool_interrupt_state> {
    return wh::adk::detail::into_owned_agent_tool_interrupt_state(std::move(value));
  }
};

} // namespace wh::core
