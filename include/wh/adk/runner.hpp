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
  event.metadata.path = append_run_path_prefix(prefix, event.metadata.path);
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
/// `compose_controls` is value-like invoke data. `compose_services` is an
/// invoke-borrowed host handle that must outlive the delegated run.
struct run_options {
  /// Compose typed controls visible to this run only.
  wh::compose::graph_invoke_controls compose_controls{};
  /// Compose typed runtime services borrowed for this run only.
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

template <typename impl_t>
[[nodiscard]] inline auto
dispatch_run(const impl_t &impl, wh::compose::graph_invoke_request request,
             wh::core::run_context &context);

[[nodiscard]] inline auto has_checkpoint_service(
    const wh::compose::graph_runtime_services *services) noexcept -> bool {
  return services != nullptr && (services->checkpoint.store != nullptr ||
                                 services->checkpoint.backend != nullptr);
}

[[nodiscard]] inline auto lower_run_request(const run_request &request)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request lowered{};
  lowered.input =
      wh::compose::graph_input::value(wh::core::any{request.messages});
  lowered.controls = request.options.compose_controls;
  lowered.services = request.options.compose_services;
  return lowered;
}

template <typename impl_t>
using runner_failure_sender_t =
    decltype(wh::core::detail::failure_result_sender<agent_run_result>(
        wh::core::errc::internal_error));

template <typename impl_t>
using runner_dispatch_sender_t =
    decltype(dispatch_run(std::declval<const impl_t &>(),
                          std::declval<wh::compose::graph_invoke_request>(),
                          std::declval<wh::core::run_context &>()));

template <typename impl_t>
using runner_sender_t = wh::core::detail::variant_sender<
    runner_failure_sender_t<impl_t>, runner_dispatch_sender_t<impl_t>>;

template <typename impl_t>
[[nodiscard]] inline auto make_runner_failure_sender(const wh::core::error_code error)
    -> runner_sender_t<impl_t> {
  return runner_sender_t<impl_t>{
      wh::core::detail::failure_result_sender<agent_run_result>(error)};
}

[[nodiscard]] inline auto lower_run_request(run_request &&request)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request lowered{};
  lowered.input = wh::compose::graph_input::value(
      wh::core::any{std::move(request.messages)});
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
    auto payload = wh::core::into_owned(target.payload);
    if (payload.has_error()) {
      return wh::core::result<void>::failure(payload.error());
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
        .data = std::move(payload).value(),
    });
  }
  return {};
}

inline auto append_resume_targets(const wh::core::run_context &context,
                                  std::vector<resume_target> &&targets,
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
  for (auto &target : targets) {
    if (target.interrupt_id.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    auto payload = wh::core::into_owned(std::move(target.payload));
    if (payload.has_error()) {
      return wh::core::result<void>::failure(payload.error());
    }
    auto location = resolve_resume_location(context, target);
    if (location.has_error()) {
      return wh::core::result<void>::failure(location.error());
    }
    auto interrupt_id = std::move(target.interrupt_id);
    controls.resume.contexts.push_back(wh::core::interrupt_context{
        .interrupt_id = interrupt_id,
        .location = std::move(location).value(),
    });
    controls.resume.batch_items.push_back(wh::compose::resume_batch_item{
        .interrupt_context_id = std::move(interrupt_id),
        .data = std::move(payload).value(),
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

template <typename impl_t, typename request_t>
  requires std::same_as<std::remove_cvref_t<request_t>, wh::adk::run_request>
[[nodiscard]] inline auto
lower_run_and_dispatch(const impl_t &impl, request_t &&request,
                       wh::core::run_context &context) {
  auto owned_request = wh::core::into_owned(std::forward<request_t>(request));
  if (owned_request.has_error()) {
    return make_runner_failure_sender<impl_t>(owned_request.error());
  }
  return runner_sender_t<impl_t>{
      dispatch_run(impl, lower_run_request(std::move(owned_request).value()), context)};
}

template <typename impl_t, typename request_t>
  requires std::same_as<std::remove_cvref_t<request_t>, wh::adk::query_request>
[[nodiscard]] inline auto
lower_query_and_dispatch(const impl_t &impl, request_t &&request,
                         wh::core::run_context &context) {
  auto owned_request = wh::core::into_owned(std::forward<request_t>(request));
  if (owned_request.has_error()) {
    return make_runner_failure_sender<impl_t>(owned_request.error());
  }
  return lower_run_and_dispatch(impl, make_run_request(std::move(owned_request).value()),
                                context);
}

template <typename impl_t, typename request_t>
  requires std::same_as<std::remove_cvref_t<request_t>, wh::adk::resume_request>
[[nodiscard]] inline auto
lower_resume_and_dispatch(const impl_t &impl, request_t &&request,
                          wh::core::run_context &context) {
  auto owned_request = wh::core::into_owned(std::forward<request_t>(request));
  if (owned_request.has_error()) {
    return make_runner_failure_sender<impl_t>(owned_request.error());
  }
  auto materialized_request = std::move(owned_request).value();

  if (materialized_request.require_checkpoint &&
      !has_checkpoint_service(materialized_request.run.options.compose_services)) {
    return make_runner_failure_sender<impl_t>(wh::core::errc::not_found);
  }

  auto lowered = lower_run_request(std::move(materialized_request.run));
  auto appended =
      append_resume_targets(context, std::move(materialized_request.targets),
                            materialized_request.reinterrupt_unmatched, lowered.controls);
  if (appended.has_error()) {
    return make_runner_failure_sender<impl_t>(appended.error());
  }

  return runner_sender_t<impl_t>{dispatch_run(impl, std::move(lowered), context)};
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
    return detail::lower_run_and_dispatch(
        impl_, std::forward<request_t>(request), context);
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, query_request>
  /// Starts one query by wrapping the query text into a single user message.
  [[nodiscard]] auto query(request_t &&request,
                           wh::core::run_context &context) const {
    return detail::lower_query_and_dispatch(
        impl_, std::forward<request_t>(request), context);
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, resume_request>
  /// Starts one resume path after lowering checkpoint and explicit
  /// target-resume controls into one compose invoke request.
  [[nodiscard]] auto resume(request_t &&request,
                            wh::core::run_context &context) const {
    return detail::lower_resume_and_dispatch(
        impl_, std::forward<request_t>(request), context);
  }

private:
  /// Stored execution implementation delegated to by all entrypoints.
  impl_t impl_{};
};

} // namespace wh::adk

namespace wh::core {

template <> struct any_owned_traits<wh::adk::resume_target> {
  [[nodiscard]] static auto into_owned(const wh::adk::resume_target &value)
      -> wh::core::result<wh::adk::resume_target> {
    auto payload = wh::core::into_owned(value.payload);
    if (payload.has_error()) {
      return wh::core::result<wh::adk::resume_target>::failure(payload.error());
    }
    return wh::adk::resume_target{
        .interrupt_id = value.interrupt_id,
        .location = value.location,
        .payload = std::move(payload).value(),
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::resume_target &&value)
      -> wh::core::result<wh::adk::resume_target> {
    auto payload = wh::core::into_owned(std::move(value.payload));
    if (payload.has_error()) {
      return wh::core::result<wh::adk::resume_target>::failure(payload.error());
    }
    return wh::adk::resume_target{
        .interrupt_id = std::move(value.interrupt_id),
        .location = std::move(value.location),
        .payload = std::move(payload).value(),
    };
  }
};

template <> struct any_owned_traits<wh::adk::run_options> {
  [[nodiscard]] static auto into_owned(const wh::adk::run_options &value)
      -> wh::core::result<wh::adk::run_options> {
    auto compose_controls = wh::core::into_owned(value.compose_controls);
    if (compose_controls.has_error()) {
      return wh::core::result<wh::adk::run_options>::failure(
          compose_controls.error());
    }
    return wh::adk::run_options{
        .compose_controls = std::move(compose_controls).value(),
        .compose_services = value.compose_services,
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::run_options &&value)
      -> wh::core::result<wh::adk::run_options> {
    auto compose_controls = wh::core::into_owned(std::move(value.compose_controls));
    if (compose_controls.has_error()) {
      return wh::core::result<wh::adk::run_options>::failure(
          compose_controls.error());
    }
    return wh::adk::run_options{
        .compose_controls = std::move(compose_controls).value(),
        .compose_services = value.compose_services,
    };
  }
};

template <> struct any_owned_traits<wh::adk::run_request> {
  [[nodiscard]] static auto into_owned(const wh::adk::run_request &value)
      -> wh::core::result<wh::adk::run_request> {
    auto options = wh::core::into_owned(value.options);
    if (options.has_error()) {
      return wh::core::result<wh::adk::run_request>::failure(options.error());
    }
    return wh::adk::run_request{
        .messages = value.messages,
        .options = std::move(options).value(),
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::run_request &&value)
      -> wh::core::result<wh::adk::run_request> {
    auto options = wh::core::into_owned(std::move(value.options));
    if (options.has_error()) {
      return wh::core::result<wh::adk::run_request>::failure(options.error());
    }
    return wh::adk::run_request{
        .messages = std::move(value.messages),
        .options = std::move(options).value(),
    };
  }
};

template <> struct any_owned_traits<wh::adk::query_request> {
  [[nodiscard]] static auto into_owned(const wh::adk::query_request &value)
      -> wh::core::result<wh::adk::query_request> {
    auto options = wh::core::into_owned(value.options);
    if (options.has_error()) {
      return wh::core::result<wh::adk::query_request>::failure(options.error());
    }
    return wh::adk::query_request{
        .text = value.text,
        .options = std::move(options).value(),
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::query_request &&value)
      -> wh::core::result<wh::adk::query_request> {
    auto options = wh::core::into_owned(std::move(value.options));
    if (options.has_error()) {
      return wh::core::result<wh::adk::query_request>::failure(options.error());
    }
    return wh::adk::query_request{
        .text = std::move(value.text),
        .options = std::move(options).value(),
    };
  }
};

template <> struct any_owned_traits<wh::adk::resume_request> {
  [[nodiscard]] static auto into_owned(const wh::adk::resume_request &value)
      -> wh::core::result<wh::adk::resume_request> {
    auto run = wh::core::into_owned(value.run);
    if (run.has_error()) {
      return wh::core::result<wh::adk::resume_request>::failure(run.error());
    }
    std::vector<wh::adk::resume_target> targets{};
    targets.reserve(value.targets.size());
    for (const auto &target : value.targets) {
      auto owned_target = wh::core::into_owned(target);
      if (owned_target.has_error()) {
        return wh::core::result<wh::adk::resume_request>::failure(
            owned_target.error());
      }
      targets.push_back(std::move(owned_target).value());
    }
    return wh::adk::resume_request{
        .run = std::move(run).value(),
        .targets = std::move(targets),
        .require_checkpoint = value.require_checkpoint,
        .reinterrupt_unmatched = value.reinterrupt_unmatched,
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::resume_request &&value)
      -> wh::core::result<wh::adk::resume_request> {
    auto run = wh::core::into_owned(std::move(value.run));
    if (run.has_error()) {
      return wh::core::result<wh::adk::resume_request>::failure(run.error());
    }
    std::vector<wh::adk::resume_target> targets{};
    targets.reserve(value.targets.size());
    for (auto &target : value.targets) {
      auto owned_target = wh::core::into_owned(std::move(target));
      if (owned_target.has_error()) {
        return wh::core::result<wh::adk::resume_request>::failure(
            owned_target.error());
      }
      targets.push_back(std::move(owned_target).value());
    }
    return wh::adk::resume_request{
        .run = std::move(run).value(),
        .targets = std::move(targets),
        .require_checkpoint = value.require_checkpoint,
        .reinterrupt_unmatched = value.reinterrupt_unmatched,
    };
  }
};

} // namespace wh::core
