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
#include "wh/core/stdexec/detail/scheduled_resume_turn.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"

namespace wh::adk::detail {

inline constexpr std::string_view agent_tool_interrupt_id_prefix = "tool:";
inline constexpr std::string_view agent_tool_interrupt_default_suffix = "interrupt";
inline constexpr std::string_view agent_tool_interrupt_reason = "agent tool interrupted";
inline constexpr std::string_view agent_tool_request_json_key = "request";
inline constexpr std::string_view agent_tool_bridge_failed_message = "agent tool bridge failed";
inline constexpr std::string_view agent_tool_history_json_schema =
    R"({"type":"object","properties":{"messages":{"type":"array","items":{"type":"object"}}},"required":["messages"]})";

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
  [[nodiscard]] static auto runtime(const agent_tool &tool)
      -> wh::core::result<agent_tool_runtime> {
    if (!tool.runtime_.has_value()) {
      return wh::core::result<agent_tool_runtime>::failure(wh::core::errc::contract_violation);
    }
    return *tool.runtime_;
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
  if (metadata.path.empty()) {
    metadata.path = run_path{{"agent", runtime.agent_name}};
  }
  metadata.path = append_run_path_prefix(scope.location, metadata.path);
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

[[nodiscard]] inline auto into_owned_bridge_state(const wh::core::any &payload)
    -> wh::core::result<wh::core::any> {
  return wh::core::into_owned(payload);
}

[[nodiscard]] inline auto into_owned_bridge_state(wh::core::any &&payload)
    -> wh::core::result<wh::core::any> {
  return wh::core::into_owned(std::move(payload));
}

[[nodiscard]] inline auto into_owned_bridge_metadata(const event_metadata &metadata)
    -> wh::core::result<event_metadata> {
  return wh::core::into_owned(metadata);
}

[[nodiscard]] inline auto into_owned_bridge_metadata(event_metadata &&metadata)
    -> wh::core::result<event_metadata> {
  return wh::core::into_owned(std::move(metadata));
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
  auto metadata = into_owned_bridge_metadata(record.metadata);
  if (metadata.has_error()) {
    return wh::core::result<agent_tool_event_record>::failure(metadata.error());
  }
  if (const auto *message = std::get_if<wh::schema::message>(&record.payload); message != nullptr) {
    return agent_tool_event_record{
        .payload = *message,
        .metadata = std::move(metadata).value(),
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
        .metadata = std::move(metadata).value(),
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
      .metadata = std::move(metadata).value(),
  };
}

[[nodiscard]] inline auto into_owned_agent_tool_event_record(agent_tool_event_record &&record)
    -> wh::core::result<agent_tool_event_record> {
  auto metadata = into_owned_bridge_metadata(std::move(record.metadata));
  if (metadata.has_error()) {
    return wh::core::result<agent_tool_event_record>::failure(metadata.error());
  }
  if (auto *message = std::get_if<wh::schema::message>(&record.payload); message != nullptr) {
    return agent_tool_event_record{
        .payload = std::move(*message),
        .metadata = std::move(metadata).value(),
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
        .metadata = std::move(metadata).value(),
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
      .metadata = std::move(metadata).value(),
  };
}

[[nodiscard]] inline auto
into_owned_agent_tool_checkpoint_state(const agent_tool_checkpoint_state &checkpoint)
    -> wh::core::result<agent_tool_checkpoint_state> {
  agent_tool_checkpoint_state owned{};
  owned.events.reserve(checkpoint.events.size());
  for (const auto &record : checkpoint.events) {
    auto owned_record = wh::core::into_owned(record);
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
    auto owned_record = wh::core::into_owned(std::move(record));
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
  auto checkpoint = wh::core::into_owned(state.checkpoint);
  if (checkpoint.has_error()) {
    return wh::core::result<agent_tool_interrupt_state>::failure(checkpoint.error());
  }

  std::optional<agent_tool_child_interrupt> child_interrupt{};
  if (state.child_interrupt.has_value()) {
    auto owned_child = wh::core::into_owned(*state.child_interrupt);
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
  auto checkpoint = wh::core::into_owned(std::move(state.checkpoint));
  if (checkpoint.has_error()) {
    return wh::core::result<agent_tool_interrupt_state>::failure(checkpoint.error());
  }

  std::optional<agent_tool_child_interrupt> child_interrupt{};
  if (state.child_interrupt.has_value()) {
    auto owned_child = wh::core::into_owned(std::move(*state.child_interrupt));
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

[[nodiscard]] inline auto
make_owned_agent_tool_checkpoint_state(const agent_tool_output_summary &output)
    -> wh::core::result<agent_tool_checkpoint_state> {
  agent_tool_checkpoint_state checkpoint{};
  checkpoint.events.reserve(output.checkpoint_events.size());
  for (const auto &record : output.checkpoint_events) {
    auto owned_record = wh::core::into_owned(record);
    if (owned_record.has_error()) {
      return wh::core::result<agent_tool_checkpoint_state>::failure(owned_record.error());
    }
    checkpoint.events.push_back(std::move(owned_record).value());
  }
  checkpoint.output_chunks = output.text_chunks;
  checkpoint.final_message = output.final_message;
  return checkpoint;
}

[[nodiscard]] inline auto make_owned_agent_tool_checkpoint_state(agent_tool_output_summary &&output)
    -> wh::core::result<agent_tool_checkpoint_state> {
  return agent_tool_checkpoint_state{
      .events = std::move(output.checkpoint_events),
      .output_chunks = std::move(output.text_chunks),
      .final_message = std::move(output.final_message),
  };
}

[[nodiscard]] inline auto make_child_interrupt_record(const wh::core::interrupt_context &context)
    -> wh::core::result<agent_tool_child_interrupt> {
  auto state = into_owned_bridge_state(context.state);
  if (state.has_error()) {
    return wh::core::result<agent_tool_child_interrupt>::failure(state.error());
  }
  auto layer_payload = into_owned_bridge_state(context.layer_payload);
  if (layer_payload.has_error()) {
    return wh::core::result<agent_tool_child_interrupt>::failure(layer_payload.error());
  }
  return agent_tool_child_interrupt{
      .interrupt_id = context.interrupt_id,
      .location = context.location,
      .state = std::move(state).value(),
      .layer_payload = std::move(layer_payload).value(),
      .parent_locations = context.parent_locations,
      .trigger_reason = context.trigger_reason,
  };
}

[[nodiscard]] inline auto to_interrupt_context(const agent_tool_child_interrupt &interrupt)
    -> wh::core::result<wh::core::interrupt_context> {
  auto owned_interrupt = wh::core::into_owned(interrupt);
  if (owned_interrupt.has_error()) {
    return wh::core::result<wh::core::interrupt_context>::failure(owned_interrupt.error());
  }
  auto materialized = std::move(owned_interrupt).value();
  return wh::core::interrupt_context{
      .interrupt_id = std::move(materialized.interrupt_id),
      .location = std::move(materialized.location),
      .state = std::move(materialized.state),
      .layer_payload = std::move(materialized.layer_payload),
      .parent_locations = std::move(materialized.parent_locations),
      .trigger_reason = std::move(materialized.trigger_reason),
  };
}

[[nodiscard]] inline auto to_interrupt_context(agent_tool_child_interrupt &&interrupt)
    -> wh::core::result<wh::core::interrupt_context> {
  auto owned_interrupt = wh::core::into_owned(std::move(interrupt));
  if (owned_interrupt.has_error()) {
    return wh::core::result<wh::core::interrupt_context>::failure(owned_interrupt.error());
  }
  auto materialized = std::move(owned_interrupt).value();
  return wh::core::interrupt_context{
      .interrupt_id = std::move(materialized.interrupt_id),
      .location = std::move(materialized.location),
      .state = std::move(materialized.state),
      .layer_payload = std::move(materialized.layer_payload),
      .parent_locations = std::move(materialized.parent_locations),
      .trigger_reason = std::move(materialized.trigger_reason),
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
  auto owned_state = wh::core::into_owned(*stored);
  if (owned_state.has_error()) {
    return wh::core::result<std::optional<agent_tool_resume_projection>>::failure(
        owned_state.error());
  }

  auto patch = wh::compose::consume_resume_data<wh::compose::resume_patch>(*context.resume_info,
                                                                           *outer_interrupt_id);
  if (patch.has_error()) {
    return wh::core::result<std::optional<agent_tool_resume_projection>>::failure(patch.error());
  }

  return std::optional<agent_tool_resume_projection>{agent_tool_resume_projection{
      .outer_interrupt_id = std::move(*outer_interrupt_id),
      .patch = std::move(patch).value(),
      .state = std::move(owned_state).value(),
  }};
}

inline auto apply_resume_projection(wh::adk::run_request &request,
                                    const agent_tool_resume_projection &projection)
    -> wh::core::result<void> {
  if (!projection.state.child_interrupt.has_value()) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }

  auto child_interrupt = to_interrupt_context(*projection.state.child_interrupt);
  if (child_interrupt.has_error()) {
    return wh::core::result<void>::failure(child_interrupt.error());
  }
  auto edited_payload = wh::core::into_owned(projection.patch.data);
  if (edited_payload.has_error()) {
    return wh::core::result<void>::failure(edited_payload.error());
  }
  request.options.compose_controls.resume.contexts.push_back(std::move(child_interrupt).value());
  request.options.compose_controls.resume.decision = wh::compose::interrupt_resume_decision{
      .interrupt_context_id = request.options.compose_controls.resume.contexts.back().interrupt_id,
      .decision = projection.patch.decision,
      .edited_payload = std::move(edited_payload).value(),
      .audit = projection.patch.audit,
  };
  return {};
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
                      const agent_tool_output_summary &output) -> wh::core::result<void> {
  if (!output.interrupted) {
    if (parent.interrupt_info.has_value() && parent.interrupt_info->location == scope.location) {
      parent.interrupt_info.reset();
    }
    return {};
  }

  std::optional<agent_tool_child_interrupt> child_interrupt{};
  if (parent.interrupt_info.has_value() && !parent.interrupt_info->interrupt_id.empty()) {
    auto captured = make_child_interrupt_record(*parent.interrupt_info);
    if (captured.has_error()) {
      return wh::core::result<void>::failure(captured.error());
    }
    child_interrupt = std::move(captured).value();
  } else if (output.child_interrupt.has_value()) {
    auto owned_child = wh::core::into_owned(*output.child_interrupt);
    if (owned_child.has_error()) {
      return wh::core::result<void>::failure(owned_child.error());
    }
    child_interrupt = std::move(owned_child).value();
  }
  auto checkpoint = make_owned_agent_tool_checkpoint_state(output);
  if (checkpoint.has_error()) {
    return wh::core::result<void>::failure(checkpoint.error());
  }
  auto outer_interrupt_id = resolve_outer_interrupt_id(runtime, scope, parent, projection,
                                                       saved_outer_interrupt, child_interrupt);
  auto checkpoint_child_interrupt = std::move(child_interrupt);
  wh::core::any layer_payload{};
  auto trigger_reason = checkpoint_child_interrupt.has_value()
                            ? checkpoint_child_interrupt->trigger_reason
                            : std::string{agent_tool_interrupt_reason};
  if (checkpoint_child_interrupt.has_value()) {
    auto copied_layer_payload = into_owned_bridge_state(checkpoint_child_interrupt->layer_payload);
    if (copied_layer_payload.has_error()) {
      return wh::core::result<void>::failure(copied_layer_payload.error());
    }
    layer_payload = std::move(copied_layer_payload).value();
  }

  parent.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = std::move(outer_interrupt_id),
      .location = scope.location,
      .state = wh::core::any(agent_tool_interrupt_state{
          .checkpoint = std::move(checkpoint).value(),
          .child_interrupt = std::move(checkpoint_child_interrupt),
      }),
      .trigger_reason = std::move(trigger_reason),
  };
  if (layer_payload.has_value()) {
    parent.interrupt_info->layer_payload = std::move(layer_payload);
  }
  return {};
}

[[nodiscard]] inline auto
build_agent_tool_request(const agent_tool_runtime &runtime, const wh::compose::tool_call &call,
                         [[maybe_unused]] const wh::core::run_context &context)
    -> wh::core::result<wh::adk::run_request> {
  switch (runtime.input_mode) {
  case agent_tool_input_mode::request: {
    auto request_payload = wh::agent::decode_tool_payload<wh::adk::agent_tool_request_arguments>(
        call.arguments);
    if (request_payload.has_error()) {
      return wh::core::result<wh::adk::run_request>::failure(request_payload.error());
    }
    wh::adk::run_request request{};
    request.messages.push_back(make_user_message(std::move(request_payload).value().request));
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
  std::optional<wh::core::interrupt_context> saved_outer_interrupt{};
  if (cleared_outer_interrupt) {
    auto owned_interrupt = wh::core::into_owned(*context.interrupt_info);
    if (owned_interrupt.has_error()) {
      return wh::core::result<agent_tool_run_setup>::failure(owned_interrupt.error());
    }
    saved_outer_interrupt = std::move(owned_interrupt).value();
  }
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

inline auto restore_outer_interrupt(wh::core::run_context &context, agent_tool_run_setup &&setup)
    -> void {
  if (!setup.cleared_outer_interrupt || context.interrupt_info.has_value()) {
    return;
  }
  context.interrupt_info = std::move(setup.saved_outer_interrupt);
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

            auto checkpoint_metadata = into_owned_bridge_metadata(normalized_metadata);
            if (checkpoint_metadata.has_error()) {
              return wh::core::result<void>::failure(checkpoint_metadata.error());
            }
            auto emitted_metadata = into_owned_bridge_metadata(normalized_metadata);
            if (emitted_metadata.has_error()) {
              return wh::core::result<void>::failure(emitted_metadata.error());
            }
            output.checkpoint_events.push_back(agent_tool_event_record{
                .payload = entry,
                .metadata = std::move(checkpoint_metadata).value(),
            });
            emitted_boundary_event = true;
            return bridge.emit(
                make_message_event(std::move(entry), std::move(emitted_metadata).value()));
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
            .location = normalized_metadata.path,
        };
        emitted_boundary_event = true;
        auto owned_metadata = into_owned_bridge_metadata(std::move(normalized_metadata));
        if (owned_metadata.has_error()) {
          return wh::core::result<void>::failure(owned_metadata.error());
        }
        auto emitted = bridge.emit(make_control_event(*action, std::move(owned_metadata).value()));
        if (emitted.has_error()) {
          return emitted;
        }
      }
      continue;
    }

    if (const auto *error = std::get_if<error_event>(&event.payload); error != nullptr) {
      output.final_error = error->code;
      auto checkpoint_detail = into_owned_bridge_state(error->detail);
      if (checkpoint_detail.has_error()) {
        return wh::core::result<void>::failure(checkpoint_detail.error());
      }
      auto checkpoint_metadata = into_owned_bridge_metadata(normalized_metadata);
      if (checkpoint_metadata.has_error()) {
        return wh::core::result<void>::failure(checkpoint_metadata.error());
      }
      auto emitted_detail = into_owned_bridge_state(error->detail);
      if (emitted_detail.has_error()) {
        return wh::core::result<void>::failure(emitted_detail.error());
      }
      auto emitted_metadata = into_owned_bridge_metadata(normalized_metadata);
      if (emitted_metadata.has_error()) {
        return wh::core::result<void>::failure(emitted_metadata.error());
      }
      output.checkpoint_events.push_back(agent_tool_event_record{
          .payload =
              agent_tool_error_event_record{
                  .code = error->code,
                  .message = error->message,
                  .detail = std::move(checkpoint_detail).value(),
              },
          .metadata = std::move(checkpoint_metadata).value(),
      });
      emitted_boundary_event = true;
      auto emitted = bridge.emit(make_error_event(error->code, error->message,
                                                  std::move(emitted_detail).value(),
                                                  std::move(emitted_metadata).value()));
      if (emitted.has_error()) {
        return emitted;
      }
      continue;
    }

    if (runtime.forward_internal_events) {
      if (const auto *custom = std::get_if<custom_event>(&event.payload); custom != nullptr) {
        auto checkpoint_payload = into_owned_bridge_state(custom->payload);
        if (checkpoint_payload.has_error()) {
          return wh::core::result<void>::failure(checkpoint_payload.error());
        }
        auto checkpoint_metadata = into_owned_bridge_metadata(normalized_metadata);
        if (checkpoint_metadata.has_error()) {
          return wh::core::result<void>::failure(checkpoint_metadata.error());
        }
        auto emitted_payload = into_owned_bridge_state(custom->payload);
        if (emitted_payload.has_error()) {
          return wh::core::result<void>::failure(emitted_payload.error());
        }
        auto emitted_metadata = into_owned_bridge_metadata(normalized_metadata);
        if (emitted_metadata.has_error()) {
          return wh::core::result<void>::failure(emitted_metadata.error());
        }
        output.checkpoint_events.push_back(agent_tool_event_record{
            .payload =
                agent_tool_custom_event_record{
                    .name = custom->name,
                    .payload = std::move(checkpoint_payload).value(),
                },
            .metadata = std::move(checkpoint_metadata).value(),
        });
        emitted_boundary_event = true;
        auto emitted = bridge.emit(make_custom_event(
            custom->name, std::move(emitted_payload).value(), std::move(emitted_metadata).value()));
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
    auto default_metadata = default_tool_metadata(runtime, scope);
    auto checkpoint_metadata = into_owned_bridge_metadata(default_metadata);
    if (checkpoint_metadata.has_error()) {
      return wh::core::result<void>::failure(checkpoint_metadata.error());
    }
    auto emitted_metadata = into_owned_bridge_metadata(std::move(default_metadata));
    if (emitted_metadata.has_error()) {
      return wh::core::result<void>::failure(emitted_metadata.error());
    }
    output.checkpoint_events.push_back(agent_tool_event_record{
        .payload = *output.final_message,
        .metadata = std::move(checkpoint_metadata).value(),
    });
    emitted_boundary_event = true;
    auto emitted =
        bridge.emit(make_message_event(*output.final_message, std::move(emitted_metadata).value()));
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
  if (!static_cast<bool>(runtime.runner.sync)) {
    return wh::core::result<agent_tool_result>::failure(wh::core::errc::not_supported);
  }

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

  auto run_result = runtime.runner.sync(setup.value().request, scope.run);
  if (run_result.has_error()) {
    restore_outer_interrupt(scope.run, std::move(setup).value());
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
    restore_outer_interrupt(scope.run, std::move(setup).value());
    return wh::core::result<agent_tool_result>::failure(materialized.error());
  }

  auto projected =
      project_child_runtime(scope.run, scope_snapshot, setup.value().saved_outer_interrupt, runtime,
                            setup.value().projection, output);
  if (projected.has_error()) {
    restore_outer_interrupt(scope.run, std::move(setup).value());
    return wh::core::result<agent_tool_result>::failure(projected.error());
  }

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
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
    class operation {
      using self_t = operation;
      using receiver_env_t =
          std::remove_cvref_t<decltype(stdexec::get_env(std::declval<const receiver_t &>()))>;
      using resume_scheduler_t = wh::core::detail::resume_scheduler_t<receiver_env_t>;
      friend class wh::core::detail::scheduled_resume_turn<self_t, resume_scheduler_t>;

      struct stopped_tag {};
      using child_completion_t =
          std::variant<event_result_t, message_result_t, wh::core::error_code, stopped_tag>;

      struct final_completion {
        std::optional<result_type> value{};
        bool stopped{false};
      };

      struct child_receiver {
        using receiver_concept = stdexec::receiver_t;

        operation *self{nullptr};
        receiver_env_t env_{};

        auto set_value(event_result_t status) && noexcept -> void {
          complete(child_completion_t{std::move(status)});
        }

        auto set_value(message_result_t status) && noexcept -> void {
          complete(child_completion_t{std::move(status)});
        }

        template <typename error_t> auto set_error(error_t &&error) && noexcept -> void {
          complete(map_async_error(std::forward<error_t>(error)));
        }

        auto set_stopped() && noexcept -> void { complete(child_completion_t{stopped_tag{}}); }

        [[nodiscard]] auto get_env() const noexcept -> receiver_env_t { return env_; }

      private:
        template <typename error_t>
        [[nodiscard]] static auto map_async_error(error_t &&error) noexcept -> child_completion_t {
          if constexpr (std::same_as<std::remove_cvref_t<error_t>, wh::core::error_code>) {
            return child_completion_t{std::forward<error_t>(error)};
          } else {
            if constexpr (std::same_as<std::remove_cvref_t<error_t>, std::exception_ptr>) {
              try {
                std::rethrow_exception(std::forward<error_t>(error));
              } catch (...) {
                return child_completion_t{wh::core::map_current_exception()};
              }
            } else {
              return child_completion_t{wh::core::make_error(wh::core::errc::internal_error)};
            }
          }
        }

        auto complete(child_completion_t completion) noexcept -> void {
          self->publish_child_completion(std::move(completion));
          self->request_resume();
          self->arrive();
        }
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
          : owner_(owner), receiver_(std::move(receiver)), env_(stdexec::get_env(receiver_)),
            scheduler_(wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env_)),
            resume_turn_(scheduler_) {}

      operation(const operation &) = delete;
      auto operator=(const operation &) -> operation & = delete;
      operation(operation &&) = delete;
      auto operator=(operation &&) -> operation & = delete;

      ~operation() {
        resume_turn_.destroy();
        destroy_child();
      }

      auto start() & noexcept -> void {
        request_resume();
        arrive();
      }

    private:
      [[nodiscard]] auto completed() const noexcept -> bool {
        return completed_.load(std::memory_order_acquire);
      }

      [[nodiscard]] auto terminal_pending() const noexcept -> bool { return terminal_.has_value(); }

      [[nodiscard]] auto child_active() const noexcept -> bool {
        return child_kind_ != child_kind::none;
      }

      [[nodiscard]] auto child_completion_ready() const noexcept -> bool {
        return child_completion_ready_.load(std::memory_order_acquire);
      }

      [[nodiscard]] auto resume_turn_completed() const noexcept -> bool { return completed(); }

      auto request_resume() noexcept -> void { resume_turn_.request(this); }

      auto arrive() noexcept -> void {
        if (count_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
          maybe_complete();
        }
      }

      auto resume_turn_arrive() noexcept -> void { arrive(); }

      auto resume_turn_add_ref() noexcept -> void {
        count_.fetch_add(1U, std::memory_order_relaxed);
      }

      auto resume_turn_schedule_error(const wh::core::error_code error) noexcept -> void {
        set_terminal_failure(error);
      }

      auto resume_turn_run() noexcept -> void {
        resume();
        maybe_complete();
      }

      auto resume_turn_idle() noexcept -> void { maybe_complete(); }

      auto maybe_complete() noexcept -> void {
        if (completed()) {
          return;
        }
        if (count_.load(std::memory_order_acquire) != 0U || !should_complete()) {
          return;
        }
        complete();
      }

      [[nodiscard]] auto should_complete() const noexcept -> bool {
        return terminal_pending() && !child_active() && !child_completion_ready() &&
               !resume_turn_.running();
      }

      auto destroy_child() noexcept -> void {
        switch (child_kind_) {
        case child_kind::none:
          return;
        case child_kind::event:
          event_child_op_.template destruct<event_child_op_t>();
          break;
        case child_kind::message:
          message_child_op_.template destruct<message_child_op_t>();
          break;
        }
        child_kind_ = child_kind::none;
      }

      auto publish_child_completion(child_completion_t completion) noexcept -> void {
        if (completed()) {
          return;
        }
        wh_invariant(!child_completion_ready());
        child_completion_.emplace(std::move(completion));
        child_completion_ready_.store(true, std::memory_order_release);
      }

      [[nodiscard]] auto drain_child_completion() noexcept -> bool {
        if (!child_completion_ready_.exchange(false, std::memory_order_acq_rel)) {
          return false;
        }
        wh_invariant(child_completion_.has_value());
        auto current = std::move(*child_completion_);
        child_completion_.reset();
        destroy_child();

        if (auto *event = std::get_if<event_result_t>(&current); event != nullptr) {
          auto mapped = owner_->process_event_result(std::move(*event));
          if (mapped.has_value()) {
            set_terminal_value(std::move(*mapped));
          }
          return true;
        }
        if (auto *message = std::get_if<message_result_t>(&current); message != nullptr) {
          auto mapped = owner_->process_active_message_result(std::move(*message));
          if (mapped.has_value()) {
            set_terminal_value(std::move(*mapped));
          }
          return true;
        }
        if (auto *error = std::get_if<wh::core::error_code>(&current); error != nullptr) {
          set_terminal_failure(*error);
          return true;
        }
        set_terminal_stopped();
        return true;
      }

      [[nodiscard]] auto start_event_child() noexcept -> bool {
        try {
          [[maybe_unused]] auto &child_op =
              event_child_op_.template construct_with<event_child_op_t>([&]() -> event_child_op_t {
                return stdexec::connect(owner_->live_events_.read_async(),
                                        child_receiver{this, env_});
              });
          child_kind_ = child_kind::event;
          count_.fetch_add(1U, std::memory_order_relaxed);
          stdexec::start(event_child_op_.template get<event_child_op_t>());
          return false;
        } catch (...) {
          destroy_child();
          set_terminal_failure(wh::core::map_current_exception());
          return true;
        }
      }

      [[nodiscard]] auto start_message_child() noexcept -> bool {
        try {
          [[maybe_unused]] auto &child_op =
              message_child_op_.template construct_with<message_child_op_t>(
                  [&]() -> message_child_op_t {
                    return stdexec::connect(owner_->active_message_reader_->read_async(),
                                            child_receiver{this, env_});
                  });
          child_kind_ = child_kind::message;
          count_.fetch_add(1U, std::memory_order_relaxed);
          stdexec::start(message_child_op_.template get<message_child_op_t>());
          return false;
        } catch (...) {
          destroy_child();
          set_terminal_failure(wh::core::map_current_exception());
          return true;
        }
      }

      auto set_terminal(final_completion completion) noexcept -> void {
        if (terminal_pending()) {
          return;
        }
        terminal_.emplace(std::move(completion));
        maybe_complete();
      }

      auto set_terminal_value(result_type status) noexcept -> void {
        set_terminal(final_completion{.value = std::move(status)});
      }

      auto set_terminal_failure(const wh::core::error_code error) noexcept -> void {
        set_terminal_value(result_type::failure(error));
      }

      auto set_terminal_stopped() noexcept -> void {
        set_terminal(final_completion{.stopped = true});
      }

      auto complete() noexcept -> void {
        if (!terminal_pending() || completed_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }

        auto completion = std::move(*terminal_);
        terminal_.reset();
        if (completion.stopped) {
          stdexec::set_stopped(std::move(receiver_));
          return;
        }
        wh_invariant(completion.value.has_value());
        stdexec::set_value(std::move(receiver_), std::move(*completion.value));
      }

      auto resume() noexcept -> void {
        while (!completed()) {
          if (drain_child_completion()) {
            continue;
          }

          if (terminal_pending()) {
            return;
          }

          if (child_active()) {
            return;
          }

          if (auto replay = owner_->poll_replay_chunk(); replay.has_value()) {
            set_terminal_value(std::move(*replay));
            continue;
          }
          if (owner_->closed_) {
            set_terminal_value(result_type{chunk_type::make_eof()});
            continue;
          }

          if (owner_->active_message_reader_.has_value()) {
            if (start_message_child()) {
              continue;
            }
            return;
          }

          if (start_event_child()) {
            continue;
          }
          return;
        }
      }

      agent_tool_live_stream_reader *owner_{nullptr};
      receiver_t receiver_;
      receiver_env_t env_{};
      resume_scheduler_t scheduler_{};
      wh::core::detail::manual_storage<sizeof(event_child_op_t), alignof(event_child_op_t)>
          event_child_op_{};
      wh::core::detail::manual_storage<sizeof(message_child_op_t), alignof(message_child_op_t)>
          message_child_op_{};
      std::optional<child_completion_t> child_completion_{};
      std::optional<final_completion> terminal_{};
      std::atomic<std::size_t> count_{1U};
      std::atomic<bool> child_completion_ready_{false};
      std::atomic<bool> completed_{false};
      wh::core::detail::scheduled_resume_turn<self_t, resume_scheduler_t> resume_turn_;
      child_kind child_kind_{child_kind::none};
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
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
      -> wh::core::result<agent_tool_child_interrupt> {
    if (context_ != nullptr && context_->interrupt_info.has_value() &&
        !context_->interrupt_info->interrupt_id.empty()) {
      return make_child_interrupt_record(*context_->interrupt_info);
    }
    return agent_tool_child_interrupt{
        .interrupt_id = action.interrupt_id,
        .location = metadata.path,
        .trigger_reason =
            action.reason.empty() ? std::string{agent_tool_interrupt_reason} : action.reason,
    };
  }

  [[nodiscard]] auto finish_interrupt(const control_action &action, const event_metadata &metadata)
      -> result_type {
    output_.interrupted = true;
    auto child_interrupt = capture_child_interrupt(action, metadata);
    if (child_interrupt.has_error()) {
      return finish_error(child_interrupt.error());
    }
    output_.child_interrupt = std::move(child_interrupt).value();
    auto projected = project_child_runtime(*context_, scope_, saved_outer_interrupt_, runtime_,
                                           projection_, output_);
    if (projected.has_error()) {
      return finish_error(projected.error());
    }
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
  if (!static_cast<bool>(runtime.runner.sync)) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::not_supported);
  }

  auto scope_snapshot = make_agent_tool_scope_snapshot(scope);
  auto setup = prepare_agent_tool_run_setup(runtime, call, scope.run, scope_snapshot);
  if (setup.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(setup.error());
  }

  auto run_result = runtime.runner.sync(setup.value().request, scope.run);
  if (run_result.has_error()) {
    restore_outer_interrupt(scope.run, std::move(setup).value());
    return wh::core::result<wh::compose::graph_stream_reader>::failure(run_result.error());
  }

  return wh::compose::graph_stream_reader{
      agent_tool_live_stream_reader{runtime, std::move(scope_snapshot), scope.run,
                                    std::move(setup).value(), std::move(run_result).value()}};
}

[[nodiscard]] inline auto open_agent_tool_result_sender(const agent_tool_runtime &runtime,
                                                        const wh::compose::tool_call &call,
                                                        const wh::tool::call_scope &scope)
    -> wh::core::detail::result_sender<wh::core::result<agent_tool_result>> {
  using result_t = wh::core::result<agent_tool_result>;

  if (!static_cast<bool>(runtime.runner.async)) {
    return wh::core::detail::result_sender<result_t>{
        wh::core::detail::failure_result_sender<result_t>(wh::core::errc::not_supported)};
  }

  const auto scope_snapshot = make_agent_tool_scope_snapshot(scope);
  auto setup = prepare_agent_tool_run_setup(runtime, call, scope.run, scope_snapshot);
  if (setup.has_error()) {
    return wh::core::detail::result_sender<result_t>{
        wh::core::detail::failure_result_sender<result_t>(setup.error())};
  }

  auto setup_state = std::move(setup).value();
  // Materialize the child request before moving the remaining setup into the continuation.
  auto request = std::move(setup_state.request);
  auto status_sender = runtime.runner.async(std::move(request), scope.run);
  return wh::core::detail::result_sender<result_t>{wh::core::detail::map_result_sender<result_t>(
      std::move(status_sender),
      [runtime, scope_snapshot, &context = scope.run,
       setup = std::move(setup_state)](agent_run_result status) mutable -> result_t {
        auto bridge = wh::adk::detail::make_live_event_bridge();
        agent_tool_output_summary output{};
        if (setup.projection.has_value()) {
          output = make_agent_tool_output_summary(setup.projection->state.checkpoint);
          auto replayed = replay_agent_tool_events(output.checkpoint_events, bridge);
          if (replayed.has_error()) {
            restore_outer_interrupt(context, std::move(setup));
            return result_t::failure(replayed.error());
          }
        }

        if (status.has_error()) {
          restore_outer_interrupt(context, std::move(setup));
          output.final_error = status.error();
          auto emitted = bridge.emit(
              make_error_event(status.error(), std::string{agent_tool_bridge_failed_message}, {},
                               default_tool_metadata(runtime, scope_snapshot)));
          if (emitted.has_error()) {
            return result_t::failure(emitted.error());
          }
          auto closed = bridge.close();
          if (closed.has_error()) {
            return result_t::failure(closed.error());
          }
          return agent_tool_result{
              .events = std::move(bridge).release_reader(),
              .final_error = status.error(),
          };
        }

        auto materialized = materialize_agent_tool_output(runtime, std::move(status).value(),
                                                          scope_snapshot, bridge, output);
        if (materialized.has_error()) {
          restore_outer_interrupt(context, std::move(setup));
          return result_t::failure(materialized.error());
        }

        auto projected = project_child_runtime(context, scope_snapshot, setup.saved_outer_interrupt,
                                               runtime, setup.projection, output);
        if (projected.has_error()) {
          restore_outer_interrupt(context, std::move(setup));
          return result_t::failure(projected.error());
        }

        std::string joined_text{};
        for (const auto &chunk : output.text_chunks) {
          joined_text.append(chunk);
        }

        auto closed = bridge.close();
        if (closed.has_error()) {
          restore_outer_interrupt(context, std::move(setup));
          return result_t::failure(closed.error());
        }

        return agent_tool_result{
            .events = std::move(bridge).release_reader(),
            .output_chunks = std::move(output.text_chunks),
            .output_text = std::move(joined_text),
            .final_message = std::move(output.final_message),
            .final_error = output.final_error,
            .interrupted = output.interrupted,
        };
      })};
}

[[nodiscard]] inline auto open_agent_tool_stream_sender(const agent_tool_runtime &runtime,
                                                        const wh::compose::tool_call &call,
                                                        const wh::tool::call_scope &scope)
    -> wh::compose::tools_stream_sender {
  using result_t = wh::core::result<wh::compose::graph_stream_reader>;

  if (!static_cast<bool>(runtime.runner.async)) {
    return wh::compose::tools_stream_sender{
        wh::core::detail::failure_result_sender<result_t>(wh::core::errc::not_supported)};
  }

  auto scope_snapshot = make_agent_tool_scope_snapshot(scope);
  auto setup = prepare_agent_tool_run_setup(runtime, call, scope.run, scope_snapshot);
  if (setup.has_error()) {
    return wh::compose::tools_stream_sender{
        wh::core::detail::failure_result_sender<result_t>(setup.error())};
  }

  auto setup_state = std::move(setup).value();
  // Materialize the child request before moving the remaining setup into the continuation.
  auto request = std::move(setup_state.request);
  auto status_sender = runtime.runner.async(std::move(request), scope.run);
  return wh::compose::tools_stream_sender{wh::core::detail::map_result_sender<result_t>(
      std::move(status_sender),
      [runtime, scope_snapshot = std::move(scope_snapshot), &context = scope.run,
       setup = std::move(setup_state)](agent_run_result status) mutable -> result_t {
        if (status.has_error()) {
          restore_outer_interrupt(context, std::move(setup));
          return result_t::failure(status.error());
        }
        return wh::compose::graph_stream_reader{
            agent_tool_live_stream_reader{runtime, std::move(scope_snapshot), context,
                                          std::move(setup), std::move(status).value()}};
      })};
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
  auto runtime = detail::agent_tool_access::runtime(*this);
  if (runtime.has_error()) {
    return wh::core::result<agent_tool_result>::failure(runtime.error());
  }
  return detail::run_agent_tool(runtime.value(), call, scope);
}

inline auto agent_tool::stream(const wh::compose::tool_call &call,
                               const wh::tool::call_scope &scope) const
    -> wh::core::result<wh::compose::graph_stream_reader> {
  auto runtime = detail::agent_tool_access::runtime(*this);
  if (runtime.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(runtime.error());
  }
  return detail::open_agent_tool_stream(runtime.value(), call, scope);
}

inline auto agent_tool::compose_entry() const -> wh::core::result<wh::compose::tool_entry> {
  auto runtime = detail::agent_tool_access::runtime(*this);
  if (runtime.has_error()) {
    return wh::core::result<wh::compose::tool_entry>::failure(runtime.error());
  }

  wh::compose::tool_entry entry{};
  if (static_cast<bool>(runtime->runner.sync)) {
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
  }

  if (static_cast<bool>(runtime->runner.async)) {
    entry.async_invoke = wh::compose::tool_async_invoke{
        [runtime =
             runtime.value()](wh::compose::tool_call call,
                              wh::tool::call_scope scope) -> wh::compose::tools_invoke_sender {
          return wh::compose::tools_invoke_sender{
              wh::core::detail::map_result_sender<wh::core::result<wh::compose::graph_value>>(
                  detail::open_agent_tool_result_sender(runtime, call, scope),
                  [](agent_tool_result result) -> wh::core::result<wh::compose::graph_value> {
                    return detail::materialize_agent_tool_value(std::move(result));
                  })};
        }};
    entry.async_stream = wh::compose::tool_async_stream{
        [runtime =
             runtime.value()](wh::compose::tool_call call,
                              wh::tool::call_scope scope) -> wh::compose::tools_stream_sender {
          return detail::open_agent_tool_stream_sender(runtime, call, scope);
        }};
    return entry;
  }

  if (static_cast<bool>(runtime->runner.sync)) {
    entry.async_invoke = wh::compose::tool_async_invoke{
        [runtime =
             runtime.value()](wh::compose::tool_call call,
                              wh::tool::call_scope scope) -> wh::compose::tools_invoke_sender {
          auto result = detail::run_agent_tool(runtime, call, scope);
          if (result.has_error()) {
            return wh::compose::tools_invoke_sender{
                wh::core::detail::failure_result_sender<wh::core::result<wh::compose::graph_value>>(
                    result.error())};
          }
          return wh::compose::tools_invoke_sender{wh::core::detail::ready_sender(
              detail::materialize_agent_tool_value(std::move(result).value()))};
        }};
    entry.async_stream = wh::compose::tool_async_stream{
        [runtime =
             runtime.value()](wh::compose::tool_call call,
                              wh::tool::call_scope scope) -> wh::compose::tools_stream_sender {
          return wh::compose::tools_stream_sender{
              wh::core::detail::ready_sender(detail::open_agent_tool_stream(runtime, call, scope))};
        }};
  }
  return entry;
}

} // namespace wh::adk

namespace wh::core {

template <> struct any_owned_traits<wh::adk::detail::agent_tool_child_interrupt> {
  [[nodiscard]] static auto into_owned(const wh::adk::detail::agent_tool_child_interrupt &value)
      -> wh::core::result<wh::adk::detail::agent_tool_child_interrupt> {
    return wh::adk::detail::into_owned_agent_tool_child_interrupt(value);
  }

  [[nodiscard]] static auto into_owned(wh::adk::detail::agent_tool_child_interrupt &&value)
      -> wh::core::result<wh::adk::detail::agent_tool_child_interrupt> {
    return wh::adk::detail::into_owned_agent_tool_child_interrupt(std::move(value));
  }
};

template <> struct any_owned_traits<wh::adk::detail::agent_tool_event_record> {
  [[nodiscard]] static auto into_owned(const wh::adk::detail::agent_tool_event_record &value)
      -> wh::core::result<wh::adk::detail::agent_tool_event_record> {
    return wh::adk::detail::into_owned_agent_tool_event_record(value);
  }

  [[nodiscard]] static auto into_owned(wh::adk::detail::agent_tool_event_record &&value)
      -> wh::core::result<wh::adk::detail::agent_tool_event_record> {
    return wh::adk::detail::into_owned_agent_tool_event_record(std::move(value));
  }
};

template <> struct any_owned_traits<wh::adk::detail::agent_tool_checkpoint_state> {
  [[nodiscard]] static auto into_owned(const wh::adk::detail::agent_tool_checkpoint_state &value)
      -> wh::core::result<wh::adk::detail::agent_tool_checkpoint_state> {
    return wh::adk::detail::into_owned_agent_tool_checkpoint_state(value);
  }

  [[nodiscard]] static auto into_owned(wh::adk::detail::agent_tool_checkpoint_state &&value)
      -> wh::core::result<wh::adk::detail::agent_tool_checkpoint_state> {
    return wh::adk::detail::into_owned_agent_tool_checkpoint_state(std::move(value));
  }
};

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
