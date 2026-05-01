// Defines chat model invocation/stream contracts and fallback execution logic
// for candidate selection, tool binding, and final error reporting.
#pragma once

#include <concepts>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/component/concepts.hpp"
#include "wh/core/component/types.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec/component_async_entry.hpp"
#include "wh/core/stdexec/inspect_result_sender.hpp"
#include "wh/core/stdexec/request_result_sender.hpp"
#include "wh/core/stdexec/resume_policy.hpp"
#include "wh/model/callback_event.hpp"
#include "wh/model/options.hpp"
#include "wh/schema/message/types.hpp"
#include "wh/schema/stream/core/any_stream.hpp"
#include "wh/schema/tool/types.hpp"

namespace wh::core {
struct run_context;
}

namespace wh::model {

/// Data contract for `chat_request`.
struct chat_request {
  /// Ordered conversation messages passed to the model.
  std::vector<wh::schema::message> messages{};
  /// Model options controlling fallback/tool/structured-output behavior.
  chat_model_options options{};
  /// Tool schema definitions available to the model.
  std::vector<wh::schema::tool_schema_definition> tools{};
};

/// Data contract for `chat_response`.
struct chat_response {
  /// Primary assistant message returned by model.
  wh::schema::message message{};
  /// Response metadata including usage and finish reason.
  wh::schema::response_meta meta{};
};

/// Streaming reader over incremental model output messages.
using chat_message_stream_reader = wh::schema::stream::any_stream_reader<wh::schema::message>;

/// Data contract for `fallback_attempt`.
struct fallback_attempt {
  /// Model type attempted for this fallback entry.
  std::string model_type{};
  /// Error returned by that fallback attempt.
  wh::core::error_code error{};
};

/// Data contract for `invoke_fallback_report`.
struct invoke_fallback_report {
  /// Successful response payload when invocation succeeds.
  chat_response response{};
  /// Attempt-level failures when failure reasons are retained.
  std::vector<fallback_attempt> attempts{};
  /// Frozen candidate order used during this invocation.
  std::vector<std::string> frozen_candidates{};
  /// Structured-output strategy negotiated for this invocation.
  structured_output_plan structured_output{};
  /// Final error when invocation fails after all attempts.
  std::optional<wh::core::error_code> final_error{};
};

/// Data contract for `stream_fallback_report`.
struct stream_fallback_report {
  /// Successful stream reader when stream startup succeeds.
  chat_message_stream_reader reader{};
  /// Model type selected for the successful stream path.
  std::string selected_model{};
  /// Attempt-level failures when failure reasons are retained.
  std::vector<fallback_attempt> attempts{};
  /// Frozen candidate order used during this stream startup.
  std::vector<std::string> frozen_candidates{};
  /// Structured-output strategy negotiated for this invocation.
  structured_output_plan structured_output{};
  /// Final error when stream startup fails after all attempts.
  std::optional<wh::core::error_code> final_error{};
};

using chat_invoke_result = wh::core::result<chat_response>;
using chat_message_stream_result = wh::core::result<chat_message_stream_reader>;

namespace detail {

using callback_sink = wh::callbacks::callback_sink;
using wh::callbacks::borrow_callback_sink;
using wh::callbacks::make_callback_sink;

template <typename... args_t> inline auto emit_callback(args_t &&...args) -> void {
  wh::callbacks::emit(std::forward<args_t>(args)...);
}

[[nodiscard]] inline auto make_run_info(const wh::core::component_descriptor &descriptor)
    -> wh::callbacks::run_info;

[[nodiscard]] inline auto make_callback_event(const wh::core::component_descriptor &descriptor,
                                              const chat_request &request, bool stream_path)
    -> chat_model_callback_event;

template <typename impl_t>
inline auto finish_stream_event(const impl_t &impl, const chat_request &request,
                                chat_model_callback_event &event) -> void;

template <typename impl_t>
[[nodiscard]] inline auto describe_impl(const impl_t &impl) -> wh::core::component_descriptor {
  if constexpr (requires {
                  { impl.descriptor() } -> std::same_as<wh::core::component_descriptor>;
                }) {
    return impl.descriptor();
  } else {
    return wh::core::component_descriptor{"ChatModel", wh::core::component_kind::model};
  }
}

template <typename impl_t>
concept sync_invoke_handler = requires(const impl_t &impl, const chat_request &request) {
  { impl.invoke(request) } -> std::same_as<chat_invoke_result>;
};

template <typename impl_t>
concept sender_invoke_handler_const = requires(const impl_t &impl, const chat_request &request) {
  { impl.invoke_sender(request) } -> stdexec::sender;
};

template <typename impl_t>
concept sender_invoke_handler_move = requires(const impl_t &impl, chat_request &&request) {
  { impl.invoke_sender(std::move(request)) } -> stdexec::sender;
};

template <typename impl_t>
concept sync_stream_handler = requires(const impl_t &impl, const chat_request &request) {
  { impl.stream(request) } -> std::same_as<chat_message_stream_result>;
};

template <typename impl_t>
concept sender_stream_handler_const = requires(const impl_t &impl, const chat_request &request) {
  { impl.stream_sender(request) } -> stdexec::sender;
};

template <typename impl_t>
concept sender_stream_handler_move = requires(const impl_t &impl, chat_request &&request) {
  { impl.stream_sender(std::move(request)) } -> stdexec::sender;
};

template <typename impl_t>
concept async_invoke_handler =
    sender_invoke_handler_const<impl_t> || sender_invoke_handler_move<impl_t>;

template <typename impl_t>
concept async_stream_handler =
    sender_stream_handler_const<impl_t> || sender_stream_handler_move<impl_t>;

template <typename impl_t>
concept sender_invoke_handler = async_invoke_handler<impl_t>;

template <typename impl_t>
concept sender_stream_handler = async_stream_handler<impl_t>;

template <typename impl_t>
concept bind_tools_handler =
    requires(const impl_t &impl, std::span<const wh::schema::tool_schema_definition> tools) {
      { impl.bind_tools(tools) } -> std::same_as<impl_t>;
    };

template <typename impl_t>
concept stream_event_hook =
    requires(const impl_t &impl, const chat_request &request, chat_model_callback_event &event) {
      { impl.finish_stream_event(request, event) } -> std::same_as<void>;
    };

template <typename impl_t>
concept model_impl = (async_invoke_handler<impl_t> || sync_invoke_handler<impl_t>) &&
                     (async_stream_handler<impl_t> || sync_stream_handler<impl_t>);

template <typename impl_t>
[[nodiscard]] inline auto run_sync_invoke_impl(const impl_t &impl, const chat_request &request)
    -> chat_invoke_result {
  if constexpr (requires {
                  { impl.invoke(request) } -> std::same_as<chat_invoke_result>;
                }) {
    return impl.invoke(request);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_invoke_impl(const impl_t &impl, chat_request &&request)
    -> chat_invoke_result {
  if constexpr (requires {
                  { impl.invoke(std::move(request)) } -> std::same_as<chat_invoke_result>;
                }) {
    return impl.invoke(std::move(request));
  } else {
    return run_sync_invoke_impl(impl, request);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_stream_impl(const impl_t &impl, const chat_request &request)
    -> chat_message_stream_result {
  return impl.stream(request);
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_stream_impl(const impl_t &impl, chat_request &&request)
    -> chat_message_stream_result {
  if constexpr (requires {
                  { impl.stream(std::move(request)) } -> std::same_as<chat_message_stream_result>;
                }) {
    return impl.stream(std::move(request));
  } else {
    return impl.stream(request);
  }
}

template <typename impl_t, typename request_t>
  requires async_invoke_handler<impl_t>
[[nodiscard]] inline auto make_invoke_sender(const impl_t &impl, request_t &&request) {
  using request_value_t = std::remove_cvref_t<request_t>;
  static_assert(std::same_as<request_value_t, chat_request>,
                "chat_model sender factory requires chat_request input");
  return wh::core::detail::request_result_sender<chat_invoke_result>(
      std::forward<request_t>(request), [&impl](auto &&forwarded_request) -> decltype(auto) {
        return impl.invoke_sender(std::forward<decltype(forwarded_request)>(forwarded_request));
      });
}

template <typename impl_t, typename request_t>
  requires async_stream_handler<impl_t>
[[nodiscard]] inline auto make_stream_sender(const impl_t &impl, request_t &&request) {
  using request_value_t = std::remove_cvref_t<request_t>;
  static_assert(std::same_as<request_value_t, chat_request>,
                "chat_model sender factory requires chat_request input");
  return wh::core::detail::request_result_sender<chat_message_stream_result>(
      std::forward<request_t>(request), [&impl](auto &&forwarded_request) -> decltype(auto) {
        return impl.stream_sender(std::forward<decltype(forwarded_request)>(forwarded_request));
      });
}

template <wh::core::resume_mode Resume, typename impl_t, typename request_t, typename scheduler_t>
  requires async_invoke_handler<impl_t>
[[nodiscard]] inline auto
invoke_sender(const impl_t &impl, const wh::core::component_descriptor &descriptor,
              request_t &&request, callback_sink sink, scheduler_t scheduler) {
  using callback_state_t = std::pair<wh::callbacks::run_info, chat_model_callback_event>;
  return wh::core::detail::component_async_entry<Resume>(
      std::forward<request_t>(request), std::move(sink), std::move(scheduler),
      [&impl](auto &&forwarded_request) {
        return make_invoke_sender(impl,
                                  std::forward<decltype(forwarded_request)>(forwarded_request));
      },
      [&descriptor](const chat_request &state_request) {
        return callback_state_t{wh::callbacks::apply_component_run_info(make_run_info(descriptor),
                                                                        state_request.options),
                                make_callback_event(descriptor, state_request, false)};
      },
      [](const callback_sink &start_sink, const callback_state_t &state) {
        emit_callback(start_sink, wh::callbacks::stage::start, state.second, state.first);
      },
      [](const callback_sink &success_sink, callback_state_t &state, chat_invoke_result &status) {
        auto &[run_info, event] = state;
        event.usage = status.value().meta.usage;
        event.emitted_chunks = 1U;
        emit_callback(success_sink, wh::callbacks::stage::end, event, run_info);
      },
      [](const callback_sink &error_sink, callback_state_t &state, chat_invoke_result &) {
        auto &[run_info, event] = state;
        emit_callback(error_sink, wh::callbacks::stage::error, event, run_info);
      });
}

template <wh::core::resume_mode Resume, typename impl_t, typename request_t, typename scheduler_t>
  requires async_stream_handler<impl_t>
[[nodiscard]] inline auto
stream_sender(const impl_t &impl, const wh::core::component_descriptor &descriptor,
              request_t &&request, callback_sink sink, scheduler_t scheduler) {
  sink = wh::callbacks::filter_callback_sink(std::move(sink), request.options);
  auto state = std::optional<std::pair<wh::callbacks::run_info, chat_model_callback_event>>{};
  if (sink.has_value()) {
    state.emplace(
        wh::callbacks::apply_component_run_info(make_run_info(descriptor), request.options),
        make_callback_event(descriptor, request, true));
  }
  return wh::core::detail::defer_result_sender<chat_message_stream_result>(
      [&impl, request = chat_request{std::forward<request_t>(request)}, sink = std::move(sink),
       state = std::move(state), scheduler = std::move(scheduler)]() mutable {
        if (state.has_value()) {
          emit_callback(sink, wh::callbacks::stage::start, state->second, state->first);
        }
        return wh::core::detail::inspect_result_sender(
            wh::core::detail::resume_if<Resume>(make_stream_sender(impl, request),
                                                std::move(scheduler)),
            [&impl, request = std::move(request), sink = std::move(sink),
             state = std::move(state)](chat_message_stream_result &status) mutable {
              if (!state.has_value()) {
                return;
              }
              auto &[run_info, event] = *state;
              if (status.has_error()) {
                emit_callback(sink, wh::callbacks::stage::error, event, run_info);
                return;
              }
              finish_stream_event(impl, request, event);
              emit_callback(sink, wh::callbacks::stage::end, event, run_info);
            });
      });
}

template <typename impl_t, typename request_t>
  requires async_invoke_handler<impl_t>
[[nodiscard]] inline auto invoke_sender(const impl_t &impl,
                                        const wh::core::component_descriptor &descriptor,
                                        request_t &&request, callback_sink sink) {
  return invoke_sender<wh::core::resume_mode::unchanged>(
      impl, descriptor, std::forward<request_t>(request), std::move(sink),
      wh::core::detail::resume_passthrough);
}

template <typename impl_t, typename request_t>
  requires async_stream_handler<impl_t>
[[nodiscard]] inline auto stream_sender(const impl_t &impl,
                                        const wh::core::component_descriptor &descriptor,
                                        request_t &&request, callback_sink sink) {
  return stream_sender<wh::core::resume_mode::unchanged>(
      impl, descriptor, std::forward<request_t>(request), std::move(sink),
      wh::core::detail::resume_passthrough);
}

template <typename impl_t>
[[nodiscard]] inline auto
bind_tools(const impl_t &impl, const std::span<const wh::schema::tool_schema_definition> tools)
    -> impl_t {
  if constexpr (requires {
                  { impl.bind_tools(tools) } -> std::same_as<impl_t>;
                }) {
    return impl.bind_tools(tools);
  } else {
    return impl;
  }
}

template <typename impl_t>
inline auto finish_stream_event([[maybe_unused]] const impl_t &impl,
                                [[maybe_unused]] const chat_request &request,
                                chat_model_callback_event &event) -> void {
  if constexpr (stream_event_hook<impl_t>) {
    impl.finish_stream_event(request, event);
  } else {
    event.emitted_chunks = 0U;
  }
}

[[nodiscard]] inline auto make_run_info(const wh::core::component_descriptor &descriptor)
    -> wh::callbacks::run_info {
  wh::callbacks::run_info run_info{};
  run_info.name = descriptor.type_name;
  run_info.type = descriptor.type_name;
  run_info.component = descriptor.kind;
  return run_info;
}

[[nodiscard]] inline auto make_callback_event(const wh::core::component_descriptor &descriptor,
                                              const chat_request &request, const bool stream_path)
    -> chat_model_callback_event {
  chat_model_callback_event event{};
  const auto options = request.options.resolve_view();
  event.model_id = options.model_id.empty() ? descriptor.type_name : std::string{options.model_id};
  event.stream_path = stream_path;
  return event;
}

} // namespace detail

/// Public interface for `chat_model`.
template <detail::model_impl impl_t,
          wh::core::resume_mode Resume = wh::core::resume_mode::unchanged>
class chat_model {
public:
  /// Default-constructs the stored implementation when supported.
  chat_model()
    requires std::default_initializable<impl_t>
  = default;

  /// Stores one chat-model implementation object by value.
  explicit chat_model(const impl_t &impl)
    requires std::copy_constructible<impl_t>
      : impl_(impl) {}

  /// Stores one movable chat-model implementation object by value.
  explicit chat_model(impl_t &&impl) noexcept(std::is_nothrow_move_constructible_v<impl_t>)
      : impl_(std::move(impl)) {}

  chat_model(const chat_model &) = default;
  chat_model(chat_model &&) noexcept = default;
  auto operator=(const chat_model &) -> chat_model & = default;
  auto operator=(chat_model &&) noexcept -> chat_model & = default;
  ~chat_model() = default;

  /// Returns static descriptor metadata for this component.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return detail::describe_impl(impl_);
  }

  /// Runs the primary request path and emits callbacks through the run context.
  [[nodiscard]] auto invoke(const chat_request &request,
                            wh::core::run_context &callback_context) const -> chat_invoke_result
    requires detail::sync_invoke_handler<impl_t>
  {
    return invoke_sync_impl(request, detail::borrow_callback_sink(callback_context));
  }

  /// Runs the primary request path with movable owning request and emits
  /// callbacks through the run context.
  [[nodiscard]] auto invoke(chat_request &&request, wh::core::run_context &callback_context) const
      -> chat_invoke_result
    requires detail::sync_invoke_handler<impl_t>
  {
    return invoke_sync_impl(std::move(request), detail::borrow_callback_sink(callback_context));
  }

  /// Starts streaming execution and emits callbacks through the run context.
  [[nodiscard]] auto stream(const chat_request &request,
                            wh::core::run_context &callback_context) const
      -> chat_message_stream_result
    requires detail::sync_stream_handler<impl_t>
  {
    return stream_sync_impl(request, detail::borrow_callback_sink(callback_context));
  }

  /// Starts streaming execution with movable owning request and emits callbacks
  /// through the run context.
  [[nodiscard]] auto stream(chat_request &&request, wh::core::run_context &callback_context) const
      -> chat_message_stream_result
    requires detail::sync_stream_handler<impl_t>
  {
    return stream_sync_impl(std::move(request), detail::borrow_callback_sink(callback_context));
  }

  /// Runs the primary request path asynchronously and emits callbacks through
  /// the run context.
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_request> &&
             detail::async_invoke_handler<impl_t>
  [[nodiscard]] auto async_invoke(request_t &&request,
                                  wh::core::run_context &callback_context) const -> auto {
    return invoke_async_impl(std::forward<request_t>(request),
                             detail::make_callback_sink(callback_context));
  }

  /// Starts streaming execution asynchronously and emits callbacks through the
  /// run context.
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_request> &&
             detail::async_stream_handler<impl_t>
  [[nodiscard]] auto async_stream(request_t &&request,
                                  wh::core::run_context &callback_context) const -> auto {
    return stream_async_impl(std::forward<request_t>(request),
                             detail::make_callback_sink(callback_context));
  }

  /// Returns a model/tool instance with the provided tool definitions bound for
  /// subsequent calls.
  [[nodiscard]] auto
  bind_tools(const std::span<const wh::schema::tool_schema_definition> tools) const -> chat_model {
    return chat_model{detail::bind_tools(impl_, tools)};
  }

  /// Returns the stored implementation object.
  [[nodiscard]] auto impl() const noexcept -> const impl_t & { return impl_; }

  /// Returns implementation-bound model options when exposed by the impl.
  [[nodiscard]] auto options() const noexcept -> decltype(auto)
    requires requires(const impl_t &impl) { impl.options(); }
  {
    return impl_.options();
  }

  /// Returns implementation-bound tool schemas when exposed by the impl.
  [[nodiscard]] auto bound_tools() const noexcept -> decltype(auto)
    requires requires(const impl_t &impl) { impl.bound_tools(); }
  {
    return impl_.bound_tools();
  }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_request> &&
             detail::sync_invoke_handler<impl_t>
  [[nodiscard]] auto invoke_sync_impl(request_t &&request, detail::callback_sink sink) const
      -> chat_invoke_result {
    const auto descriptor = this->descriptor();
    sink = wh::callbacks::filter_callback_sink(std::move(sink), request.options);
    const auto run_info =
        wh::callbacks::apply_component_run_info(detail::make_run_info(descriptor), request.options);
    auto event = detail::make_callback_event(descriptor, request, false);
    detail::emit_callback(sink, wh::callbacks::stage::start, event, run_info);

    auto output = detail::run_sync_invoke_impl(impl_, std::forward<request_t>(request));
    if (output.has_error()) {
      detail::emit_callback(sink, wh::callbacks::stage::error, event, run_info);
      return output;
    }

    event.usage = output.value().meta.usage;
    event.emitted_chunks = 1U;
    detail::emit_callback(sink, wh::callbacks::stage::end, event, run_info);
    return output;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_request> &&
             detail::sync_stream_handler<impl_t>
  [[nodiscard]] auto stream_sync_impl(request_t &&request, detail::callback_sink sink) const
      -> chat_message_stream_result {
    const auto descriptor = this->descriptor();
    sink = wh::callbacks::filter_callback_sink(std::move(sink), request.options);
    const auto run_info =
        wh::callbacks::apply_component_run_info(detail::make_run_info(descriptor), request.options);
    auto event = detail::make_callback_event(descriptor, request, true);
    detail::emit_callback(sink, wh::callbacks::stage::start, event, run_info);

    auto output = detail::run_sync_stream_impl(impl_, std::forward<request_t>(request));
    if (output.has_error()) {
      detail::emit_callback(sink, wh::callbacks::stage::error, event, run_info);
      return output;
    }

    detail::finish_stream_event(impl_, request, event);
    detail::emit_callback(sink, wh::callbacks::stage::end, event, run_info);
    return output;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_request> &&
             detail::async_invoke_handler<impl_t>
  [[nodiscard]] auto invoke_async_impl(request_t &&request, detail::callback_sink sink) const
      -> auto {
    const auto descriptor = this->descriptor();
    return wh::core::detail::defer_resume_sender<Resume>(
        [this, request = chat_request{std::forward<request_t>(request)}, sink = std::move(sink),
         descriptor](auto scheduler) mutable {
          return detail::invoke_sender<Resume>(impl_, descriptor, std::move(request),
                                               std::move(sink), std::move(scheduler));
        });
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_request> &&
             detail::async_stream_handler<impl_t>
  [[nodiscard]] auto stream_async_impl(request_t &&request, detail::callback_sink sink) const
      -> auto {
    const auto descriptor = this->descriptor();
    return wh::core::detail::defer_resume_sender<Resume>(
        [this, request = chat_request{std::forward<request_t>(request)}, sink = std::move(sink),
         descriptor](auto scheduler) mutable {
          return detail::stream_sender<Resume>(impl_, descriptor, std::move(request),
                                               std::move(sink), std::move(scheduler));
        });
  }

  wh_no_unique_address impl_t impl_{};
};

template <typename impl_t> chat_model(impl_t &&) -> chat_model<std::remove_cvref_t<impl_t>>;

template <typename model_t>
concept chat_model_like =
    wh::core::component_descriptor_provider<model_t> &&
    wh::core::invokable_component<const model_t &, chat_request, chat_response> &&
    wh::core::streamable_component<const model_t &, chat_request, chat_message_stream_reader> &&
    requires(const model_t &model, chat_request &&movable_request,
             std::span<const wh::schema::tool_schema_definition> tools,
             wh::core::run_context &context) {
      { model.invoke(std::move(movable_request), context) } -> std::same_as<chat_invoke_result>;
      {
        model.stream(std::move(movable_request), context)
      } -> std::same_as<chat_message_stream_result>;
      { model.bind_tools(tools) } -> std::same_as<std::remove_cvref_t<model_t>>;
    };

namespace detail {

template <typename model_t, typename request_t>
[[nodiscard]] inline auto
invoke_with_fallback_report_only_impl(const std::span<const model_t> models, request_t &&request,
                                      const bool provider_native_supported = false)
    -> invoke_fallback_report {
  invoke_fallback_report report{};
  if (models.empty()) {
    report.final_error = wh::core::make_error(wh::core::errc::not_found);
    return report;
  }

  chat_request effective_request = std::forward<request_t>(request);
  const auto resolved_options = effective_request.options.resolve_view();
  std::vector<std::string> discovered_candidates{};
  discovered_candidates.reserve(models.size());
  for (const auto &model : models) {
    const auto &type_name = model.descriptor().type_name;
    if (std::ranges::find(discovered_candidates, type_name) == discovered_candidates.end()) {
      discovered_candidates.push_back(type_name);
    }
  }

  auto frozen_candidates = freeze_model_candidates(resolved_options, discovered_candidates);
  const auto structured_output = negotiate_structured_output(
      resolved_options, provider_native_supported, !effective_request.tools.empty());
  const bool keep_failure_reasons = resolved_options.fallback_ref().keep_failure_reasons;
  report.frozen_candidates = frozen_candidates;
  report.structured_output = structured_output;

  std::vector<wh::schema::tool_schema_definition> effective_tools = effective_request.tools;
  if (resolved_options.tool_choice_ref().mode == wh::schema::tool_call_mode::disable) {
    effective_tools.clear();
  } else {
    if (!resolved_options.allowed_tool_names.empty()) {
      std::vector<wh::schema::tool_schema_definition> filtered_tools{};
      filtered_tools.reserve(effective_tools.size());
      for (const auto &tool : effective_tools) {
        if (std::ranges::find(resolved_options.allowed_tool_names, tool.name) !=
            resolved_options.allowed_tool_names.end()) {
          filtered_tools.push_back(tool);
        }
      }
      effective_tools = std::move(filtered_tools);
    }

    if (resolved_options.tool_choice_ref().mode == wh::schema::tool_call_mode::force &&
        effective_tools.empty()) {
      if (effective_request.tools.empty()) {
        report.final_error = wh::core::make_error(wh::core::errc::invalid_argument);
      } else {
        report.final_error = wh::core::make_error(wh::core::errc::not_found);
      }
      return report;
    }
  }

  effective_request.tools = effective_tools;

  std::vector<const model_t *> ordered_models{};
  ordered_models.reserve(models.size());
  std::vector<bool> consumed(models.size(), false);
  for (const auto &candidate : frozen_candidates) {
    for (std::size_t index = 0U; index < models.size(); ++index) {
      if (consumed[index]) {
        continue;
      }
      if (models[index].descriptor().type_name != candidate) {
        continue;
      }
      ordered_models.push_back(std::addressof(models[index]));
      consumed[index] = true;
      break;
    }
  }
  for (std::size_t index = 0U; index < models.size(); ++index) {
    if (consumed[index]) {
      continue;
    }
    ordered_models.push_back(std::addressof(models[index]));
  }

  report.attempts.reserve(ordered_models.size());
  wh::core::error_code last_error = wh::core::make_error(wh::core::errc::not_found);
  const auto record_attempt = [&]<typename model_type_t>(model_type_t &&model_type,
                                                         const wh::core::error_code error) {
    static_assert(std::constructible_from<std::string, model_type_t &&>,
                  "model_type must be constructible as std::string");
    if (!keep_failure_reasons) {
      return;
    }
    report.attempts.push_back(
        fallback_attempt{std::string{std::forward<model_type_t>(model_type)}, error});
  };

  for (const model_t *model : ordered_models) {
    const model_t *invokable = model;
    std::optional<model_t> bound_invokable{};
    if (!effective_request.tools.empty()) {
      bound_invokable.emplace(invokable->bind_tools(effective_request.tools));
      invokable = std::addressof(*bound_invokable);
    }

    wh::core::run_context callback_context{};
    auto invoked = invokable->invoke(effective_request, callback_context);
    if (invoked.has_value()) {
      report.response = std::move(invoked).value();
      return report;
    }

    last_error = invoked.error();
    record_attempt(invokable->descriptor().type_name, last_error);
  }

  report.final_error = last_error;
  return report;
}

template <typename model_t, typename request_t>
[[nodiscard]] inline auto
stream_with_fallback_report_only_impl(const std::span<const model_t> models, request_t &&request,
                                      const bool provider_native_supported = false)
    -> stream_fallback_report {
  stream_fallback_report report{};
  if (models.empty()) {
    report.final_error = wh::core::make_error(wh::core::errc::not_found);
    return report;
  }

  chat_request effective_request = std::forward<request_t>(request);
  const auto resolved_options = effective_request.options.resolve_view();
  std::vector<std::string> discovered_candidates{};
  discovered_candidates.reserve(models.size());
  for (const auto &model : models) {
    const auto &type_name = model.descriptor().type_name;
    if (std::ranges::find(discovered_candidates, type_name) == discovered_candidates.end()) {
      discovered_candidates.push_back(type_name);
    }
  }

  auto frozen_candidates = freeze_model_candidates(resolved_options, discovered_candidates);
  const auto structured_output = negotiate_structured_output(
      resolved_options, provider_native_supported, !effective_request.tools.empty());
  const bool keep_failure_reasons = resolved_options.fallback_ref().keep_failure_reasons;
  report.frozen_candidates = frozen_candidates;
  report.structured_output = structured_output;

  std::vector<wh::schema::tool_schema_definition> effective_tools = effective_request.tools;
  if (resolved_options.tool_choice_ref().mode == wh::schema::tool_call_mode::disable) {
    effective_tools.clear();
  } else {
    if (!resolved_options.allowed_tool_names.empty()) {
      std::vector<wh::schema::tool_schema_definition> filtered_tools{};
      filtered_tools.reserve(effective_tools.size());
      for (const auto &tool : effective_tools) {
        if (std::ranges::find(resolved_options.allowed_tool_names, tool.name) !=
            resolved_options.allowed_tool_names.end()) {
          filtered_tools.push_back(tool);
        }
      }
      effective_tools = std::move(filtered_tools);
    }

    if (resolved_options.tool_choice_ref().mode == wh::schema::tool_call_mode::force &&
        effective_tools.empty()) {
      if (effective_request.tools.empty()) {
        report.final_error = wh::core::make_error(wh::core::errc::invalid_argument);
      } else {
        report.final_error = wh::core::make_error(wh::core::errc::not_found);
      }
      return report;
    }
  }

  effective_request.tools = effective_tools;

  std::vector<const model_t *> ordered_models{};
  ordered_models.reserve(models.size());
  std::vector<bool> consumed(models.size(), false);
  for (const auto &candidate : frozen_candidates) {
    for (std::size_t index = 0U; index < models.size(); ++index) {
      if (consumed[index]) {
        continue;
      }
      if (models[index].descriptor().type_name != candidate) {
        continue;
      }
      ordered_models.push_back(std::addressof(models[index]));
      consumed[index] = true;
      break;
    }
  }
  for (std::size_t index = 0U; index < models.size(); ++index) {
    if (consumed[index]) {
      continue;
    }
    ordered_models.push_back(std::addressof(models[index]));
  }

  report.attempts.reserve(ordered_models.size());
  wh::core::error_code last_error = wh::core::make_error(wh::core::errc::not_found);
  const auto record_attempt = [&]<typename model_type_t>(model_type_t &&model_type,
                                                         const wh::core::error_code error) {
    static_assert(std::constructible_from<std::string, model_type_t &&>,
                  "model_type must be constructible as std::string");
    if (!keep_failure_reasons) {
      return;
    }
    report.attempts.push_back(
        fallback_attempt{std::string{std::forward<model_type_t>(model_type)}, error});
  };

  for (const model_t *model : ordered_models) {
    const model_t *invokable = model;
    std::optional<model_t> bound_invokable{};
    if (!effective_request.tools.empty()) {
      bound_invokable.emplace(invokable->bind_tools(effective_request.tools));
      invokable = std::addressof(*bound_invokable);
    }

    wh::core::run_context callback_context{};
    auto streamed = invokable->stream(effective_request, callback_context);
    if (streamed.has_value()) {
      report.reader = std::move(streamed).value();
      report.selected_model = invokable->descriptor().type_name;
      return report;
    }

    last_error = streamed.error();
    record_attempt(invokable->descriptor().type_name, last_error);
  }

  report.final_error = last_error;
  return report;
}

} // namespace detail

template <chat_model_like model_t>
[[nodiscard]] inline auto
invoke_with_fallback_report_only(const std::span<const model_t> models, const chat_request &request,
                                 const bool provider_native_supported = false)
    -> invoke_fallback_report {
  return detail::invoke_with_fallback_report_only_impl<model_t>(models, request,
                                                                provider_native_supported);
}

template <chat_model_like model_t>
[[nodiscard]] inline auto
invoke_with_fallback_report_only(const std::span<const model_t> models, chat_request &&request,
                                 const bool provider_native_supported = false)
    -> invoke_fallback_report {
  return detail::invoke_with_fallback_report_only_impl<model_t>(models, std::move(request),
                                                                provider_native_supported);
}

template <chat_model_like model_t>
[[nodiscard]] inline auto invoke_with_fallback(const std::span<const model_t> models,
                                               const chat_request &request,
                                               const bool provider_native_supported = false)
    -> wh::core::result<invoke_fallback_report> {
  auto report = invoke_with_fallback_report_only(models, request, provider_native_supported);
  if (report.final_error.has_value()) {
    return wh::core::result<invoke_fallback_report>::failure(*report.final_error);
  }
  return report;
}

template <chat_model_like model_t>
[[nodiscard]] inline auto invoke_with_fallback(const std::span<const model_t> models,
                                               chat_request &&request,
                                               const bool provider_native_supported = false)
    -> wh::core::result<invoke_fallback_report> {
  auto report =
      invoke_with_fallback_report_only(models, std::move(request), provider_native_supported);
  if (report.final_error.has_value()) {
    return wh::core::result<invoke_fallback_report>::failure(*report.final_error);
  }
  return report;
}

template <chat_model_like model_t>
[[nodiscard]] inline auto
stream_with_fallback_report_only(const std::span<const model_t> models, const chat_request &request,
                                 const bool provider_native_supported = false)
    -> stream_fallback_report {
  return detail::stream_with_fallback_report_only_impl<model_t>(models, request,
                                                                provider_native_supported);
}

template <chat_model_like model_t>
[[nodiscard]] inline auto
stream_with_fallback_report_only(const std::span<const model_t> models, chat_request &&request,
                                 const bool provider_native_supported = false)
    -> stream_fallback_report {
  return detail::stream_with_fallback_report_only_impl<model_t>(models, std::move(request),
                                                                provider_native_supported);
}

template <chat_model_like model_t>
[[nodiscard]] inline auto stream_with_fallback(const std::span<const model_t> models,
                                               const chat_request &request,
                                               const bool provider_native_supported = false)
    -> wh::core::result<stream_fallback_report> {
  auto report = stream_with_fallback_report_only(models, request, provider_native_supported);
  if (report.final_error.has_value()) {
    return wh::core::result<stream_fallback_report>::failure(*report.final_error);
  }
  return report;
}

template <chat_model_like model_t>
[[nodiscard]] inline auto stream_with_fallback(const std::span<const model_t> models,
                                               chat_request &&request,
                                               const bool provider_native_supported = false)
    -> wh::core::result<stream_fallback_report> {
  auto report =
      stream_with_fallback_report_only(models, std::move(request), provider_native_supported);
  if (report.final_error.has_value()) {
    return wh::core::result<stream_fallback_report>::failure(*report.final_error);
  }
  return report;
}

} // namespace wh::model
