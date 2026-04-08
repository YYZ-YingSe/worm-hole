// Defines the ADK runner bridge that lowers Run/Query/Resume requests onto
// compose typed invoke requests without introducing a second runtime.
#pragma once

#include <concepts>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/adk/event_stream.hpp"
#include "wh/compose/graph/invoke_types.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/stream/algorithm.hpp"

namespace wh::adk {

/// Shared run output shape used by workflow and agent-family wrappers.
struct agent_run_output {
  /// Event stream produced by the wrapped agent execution.
  agent_event_stream_reader events{};
  /// Final message observed on the wrapped execution path when available.
  std::optional<wh::schema::message> final_message{};
  /// Output values explicitly materialized by the wrapped execution.
  std::unordered_map<std::string, wh::core::any,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      output_values{};
};

/// Canonical success/failure boundary for one wrapped agent execution.
using agent_run_result = wh::core::result<agent_run_output>;

/// Concatenates two run paths without mutating either source path.
[[nodiscard]] inline auto append_run_path_prefix(const run_path &prefix,
                                                 const run_path &suffix)
    -> run_path {
  run_path combined = prefix;
  for (const auto &segment : suffix.segments()) {
    combined = combined.append(segment);
  }
  return combined;
}

/// Prefixes one event run path with the supplied parent path.
[[nodiscard]] inline auto prefix_agent_event(agent_event event,
                                             const run_path &prefix)
    -> agent_event {
  event.metadata.run_path =
      append_run_path_prefix(prefix, event.metadata.run_path);
  return event;
}

/// Collects one message reader into owned messages.
[[nodiscard]] inline auto
collect_agent_messages(agent_message_stream_reader &&reader)
    -> wh::core::result<std::vector<wh::schema::message>> {
  return wh::schema::stream::collect_stream_reader(std::move(reader));
}

/// Collects one event reader into owned events.
[[nodiscard]] inline auto
collect_agent_events(agent_event_stream_reader &&reader)
    -> wh::core::result<std::vector<agent_event>> {
  return wh::schema::stream::collect_stream_reader(std::move(reader));
}

/// Returns the last owned message carried by one event sequence, when any.
[[nodiscard]] inline auto
find_final_message(const std::vector<agent_event> &events)
    -> std::optional<wh::schema::message> {
  for (auto iter = events.rbegin(); iter != events.rend(); ++iter) {
    const auto *message = std::get_if<message_event>(&iter->payload);
    if (message == nullptr) {
      continue;
    }
    if (const auto *value = std::get_if<wh::schema::message>(&message->content);
        value != nullptr) {
      return *value;
    }
  }
  return std::nullopt;
}

/// Per-run controls lowered by the runner before delegating to one execution
/// implementation.
struct run_options {
  /// Compose typed controls visible to this run only.
  wh::compose::graph_invoke_controls compose_controls{};
  /// Compose typed runtime services visible to this run only.
  const wh::compose::graph_runtime_services *compose_services{nullptr};
};

/// Input payload for one runner `run` entrypoint.
struct run_request {
  /// Ordered input messages passed to the underlying ADK implementation.
  std::vector<wh::schema::message> messages{};
  /// Per-run lowering controls materialized by the runner.
  run_options options{};
};

/// Input payload for one runner `query` entrypoint.
struct query_request {
  /// Single user text wrapped into one user-role message before execution.
  std::string text{};
  /// Per-run lowering controls materialized by the runner.
  run_options options{};
};

/// One explicit resume target keyed by interrupt-context id.
struct resume_target {
  /// Stable interrupt-context id used by restore routing.
  std::string interrupt_id{};
  /// Optional explicit location for this target.
  std::optional<wh::core::address> location{};
  /// Resume payload injected for this interrupt target.
  wh::core::any payload{};
};

/// Input payload for one runner `resume` entrypoint.
struct resume_request {
  /// Base run request used once restore controls are injected.
  run_request run{};
  /// Explicit target-resume payloads keyed by interrupt-context id.
  std::vector<resume_target> targets{};
  /// True requires checkpoint services before resume starts.
  bool require_checkpoint{true};
  /// True re-interrupts unmatched contexts after applying explicit targets.
  bool reinterrupt_unmatched{false};
};

/// Builds one `run_request` from one `query_request` by wrapping the text into
/// a single user-role message.
[[nodiscard]] inline auto make_run_request(query_request request)
    -> run_request {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{std::move(request.text)});
  return run_request{
      .messages = std::vector<wh::schema::message>{std::move(message)},
      .options = std::move(request.options),
  };
}

namespace detail {

template <typename impl_t>
concept async_runner_handler_const =
    requires(const impl_t &impl,
             const wh::compose::graph_invoke_request &request,
             wh::core::run_context &context) {
      requires wh::compose::result_typed_sender<
          decltype(impl.run(request, context)), agent_run_result>;
    };

template <typename impl_t>
concept async_runner_handler_move =
    requires(const impl_t &impl, wh::compose::graph_invoke_request &&request,
             wh::core::run_context &context) {
      requires wh::compose::result_typed_sender<
          decltype(impl.run(std::move(request), context)), agent_run_result>;
    };

template <typename impl_t>
concept runner_impl =
    async_runner_handler_const<impl_t> || async_runner_handler_move<impl_t>;

[[nodiscard]] inline auto has_checkpoint_service(
    const wh::compose::graph_runtime_services *services) noexcept -> bool {
  return services != nullptr && (services->checkpoint.store != nullptr ||
                                 services->checkpoint.backend != nullptr);
}

[[nodiscard]] inline auto lower_run_request(const run_request &request)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request lowered{};
  lowered.input = wh::core::any{request.messages};
  lowered.controls = request.options.compose_controls;
  lowered.services = request.options.compose_services;
  return lowered;
}

[[nodiscard]] inline auto lower_run_request(run_request &&request)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request lowered{};
  lowered.input = wh::core::any{std::move(request.messages)};
  lowered.controls = std::move(request.options.compose_controls);
  lowered.services = request.options.compose_services;
  return lowered;
}

[[nodiscard]] inline auto
resolve_resume_location_one(const wh::core::run_context &context,
                            const std::string_view interrupt_id)
    -> wh::core::result<wh::core::address> {
  if (context.resume_info.has_value()) {
    auto location = context.resume_info->location_of(interrupt_id);
    if (location.has_value()) {
      return location.value().get();
    }
    if (location.error() != wh::core::errc::not_found) {
      return wh::core::result<wh::core::address>::failure(location.error());
    }
  }

  if (context.interrupt_info.has_value() &&
      context.interrupt_info->interrupt_id == interrupt_id) {
    return context.interrupt_info->location;
  }

  return wh::core::result<wh::core::address>::failure(
      wh::core::errc::not_found);
}

[[nodiscard]] inline auto
resolve_resume_location(const wh::core::run_context &context,
                        const resume_target &target)
    -> wh::core::result<wh::core::address> {
  if (target.location.has_value()) {
    return *target.location;
  }
  return resolve_resume_location_one(context, target.interrupt_id);
}

inline auto append_resume_targets(const wh::core::run_context &context,
                                  const std::vector<resume_target> &targets,
                                  const bool reinterrupt_unmatched,
                                  wh::compose::graph_invoke_controls &controls)
    -> wh::core::result<void> {
  controls.resume.reinterrupt_unmatched = reinterrupt_unmatched;
  if (targets.empty()) {
    return {};
  }

  controls.resume.batch_items.reserve(controls.resume.batch_items.size() +
                                      targets.size());
  controls.resume.contexts.reserve(controls.resume.contexts.size() +
                                   targets.size());
  for (const auto &target : targets) {
    if (target.interrupt_id.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    auto location = resolve_resume_location(context, target);
    if (location.has_error()) {
      return wh::core::result<void>::failure(location.error());
    }
    controls.resume.contexts.push_back(wh::core::interrupt_context{
        .interrupt_id = target.interrupt_id,
        .location = std::move(location).value(),
    });
    controls.resume.batch_items.push_back(wh::compose::resume_batch_item{
        .interrupt_context_id = target.interrupt_id,
        .data = target.payload,
    });
  }
  return {};
}

template <typename impl_t>
[[nodiscard]] inline auto
dispatch_run(const impl_t &impl, wh::compose::graph_invoke_request request,
             wh::core::run_context &context) {
  if constexpr (async_runner_handler_move<impl_t>) {
    return wh::core::detail::normalize_result_sender<agent_run_result>(
        impl.run(std::move(request), context));
  } else {
    return wh::core::detail::normalize_result_sender<agent_run_result>(impl.run(
        static_cast<const wh::compose::graph_invoke_request &>(request),
        context));
  }
}

template <typename impl_t>
[[nodiscard]] inline auto
lower_resume_and_dispatch(const impl_t &impl, resume_request request,
                          wh::core::run_context &context) {
  using failure_sender_t =
      decltype(wh::core::detail::failure_result_sender<agent_run_result>(
          wh::core::errc::internal_error));
  using dispatch_sender_t =
      decltype(dispatch_run(std::declval<const impl_t &>(),
                            std::declval<wh::compose::graph_invoke_request>(),
                            std::declval<wh::core::run_context &>()));
  using resume_sender_t =
      wh::core::detail::variant_sender<failure_sender_t, dispatch_sender_t>;

  if (request.require_checkpoint &&
      !has_checkpoint_service(request.run.options.compose_services)) {
    return resume_sender_t{
        wh::core::detail::failure_result_sender<agent_run_result>(
            wh::core::errc::not_found)};
  }

  auto lowered = lower_run_request(std::move(request.run));
  auto appended =
      append_resume_targets(context, request.targets,
                            request.reinterrupt_unmatched, lowered.controls);
  if (appended.has_error()) {
    return resume_sender_t{
        wh::core::detail::failure_result_sender<agent_run_result>(
            appended.error())};
  }

  return resume_sender_t{dispatch_run(impl, std::move(lowered), context)};
}

} // namespace detail

/// Sender-first bridge that materializes ADK run semantics into one compose
/// typed invoke request and delegates execution to one lower implementation.
template <detail::runner_impl impl_t> class runner {
public:
  runner() = default;

  /// Stores the underlying execution implementation by value.
  explicit runner(impl_t impl) noexcept(
      std::is_nothrow_move_constructible_v<impl_t>)
      : impl_(std::move(impl)) {}

  runner(const runner &) = default;
  runner(runner &&) noexcept = default;
  auto operator=(const runner &) -> runner & = default;
  auto operator=(runner &&) noexcept -> runner & = default;
  ~runner() = default;

  /// Returns the stored implementation for tests and lower-layer integration.
  [[nodiscard]] auto implementation() const noexcept -> const impl_t & {
    return impl_;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, run_request>
  /// Starts one run by lowering ADK input to one compose invoke request.
  [[nodiscard]] auto run(request_t &&request,
                         wh::core::run_context &context) const {
    return detail::dispatch_run(
        impl_, detail::lower_run_request(std::forward<request_t>(request)),
        context);
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, query_request>
  /// Starts one query by wrapping the query text into a single user message.
  [[nodiscard]] auto query(request_t &&request,
                           wh::core::run_context &context) const {
    return run(make_run_request(std::forward<request_t>(request)), context);
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, resume_request>
  /// Starts one resume path after lowering checkpoint and explicit
  /// target-resume controls into one compose invoke request.
  [[nodiscard]] auto resume(request_t &&request,
                            wh::core::run_context &context) const {
    return detail::lower_resume_and_dispatch(
        impl_, resume_request{std::forward<request_t>(request)}, context);
  }

private:
  /// Stored execution implementation delegated to by all entrypoints.
  impl_t impl_{};
};

} // namespace wh::adk
