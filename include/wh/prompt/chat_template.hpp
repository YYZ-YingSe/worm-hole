// Defines the prompt rendering contract that converts template context into
// ordered chat messages for downstream model invocation.
#pragma once

#include <concepts>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/component/concepts.hpp"
#include "wh/core/component/types.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec/component_async_entry.hpp"
#include "wh/core/stdexec/request_result_sender.hpp"
#include "wh/core/stdexec/resume_policy.hpp"
#include "wh/prompt/callback_event.hpp"
#include "wh/prompt/options.hpp"
#include "wh/prompt/template.hpp"
#include "wh/schema/message/types.hpp"

namespace wh::core {
struct run_context;
}

namespace wh::prompt {

struct prompt_render_request {
  template_context context{};
  prompt_options options{};
};

namespace detail {

using prompt_result = wh::core::result<std::vector<wh::schema::message>>;
struct callback_state {
  wh::callbacks::run_info run_info{};
  prompt_callback_event event{};
};

using callback_sink = wh::callbacks::callback_sink;
using wh::callbacks::borrow_callback_sink;
using wh::callbacks::make_callback_sink;

template <typename... args_t> inline auto emit_callback(args_t &&...args) -> void {
  wh::callbacks::emit(std::forward<args_t>(args)...);
}

[[nodiscard]] inline auto make_callback_state(const prompt_render_request &request)
    -> callback_state {
  callback_state state{};
  state.run_info.name = "Prompt";
  state.run_info.type = "Prompt";
  state.run_info.component = wh::core::component_kind::prompt;
  state.run_info =
      wh::callbacks::apply_component_run_info(std::move(state.run_info), request.options);
  const auto options = request.options.resolve_view();
  state.event.template_name = options.template_name;
  state.event.variable_count = request.context.size();
  return state;
}

template <typename impl_t>
concept sync_prompt_handler = requires(const impl_t &impl, const prompt_render_request &request,
                                       prompt_callback_event &event) {
  { impl.render(request, event) } -> std::same_as<prompt_result>;
} || requires(const impl_t &impl, const prompt_render_request &request) {
  { impl.render(request) } -> std::same_as<prompt_result>;
};

template <typename impl_t>
concept sender_prompt_handler_const =
    requires(const impl_t &impl, const prompt_render_request &request) {
      { impl.render_sender(request) } -> stdexec::sender;
    };

template <typename impl_t>
concept sender_prompt_handler_move = requires(const impl_t &impl, prompt_render_request &&request) {
  { impl.render_sender(std::move(request)) } -> stdexec::sender;
};

template <typename impl_t>
concept async_prompt_handler =
    sender_prompt_handler_const<impl_t> || sender_prompt_handler_move<impl_t>;

template <typename impl_t>
concept sender_prompt_handler = async_prompt_handler<impl_t>;

template <typename impl_t>
concept chat_template_impl = async_prompt_handler<impl_t> || sync_prompt_handler<impl_t>;

template <typename impl_t>
[[nodiscard]] inline auto run_sync_prompt_impl(const impl_t &impl,
                                               const prompt_render_request &request,
                                               prompt_callback_event &event) -> prompt_result {
  if constexpr (requires {
                  { impl.render(request, event) } -> std::same_as<prompt_result>;
                }) {
    return impl.render(request, event);
  } else {
    return impl.render(request);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_prompt_impl(const impl_t &impl, prompt_render_request &&request,
                                               prompt_callback_event &event) -> prompt_result {
  if constexpr (requires {
                  { impl.render(std::move(request), event) } -> std::same_as<prompt_result>;
                }) {
    return impl.render(std::move(request), event);
  } else if constexpr (requires {
                         { impl.render(std::move(request)) } -> std::same_as<prompt_result>;
                       }) {
    return impl.render(std::move(request));
  } else {
    return run_sync_prompt_impl(impl, request, event);
  }
}

template <typename impl_t, typename request_t>
  requires async_prompt_handler<impl_t>
[[nodiscard]] inline auto make_render_sender(const impl_t &impl, request_t &&request) {
  using request_value_t = std::remove_cvref_t<request_t>;
  static_assert(std::same_as<request_value_t, prompt_render_request>,
                "prompt sender factory requires prompt_render_request input");
  return wh::core::detail::request_result_sender<prompt_result>(
      std::forward<request_t>(request), [&impl](auto &&forwarded_request) -> decltype(auto) {
        return impl.render_sender(std::forward<decltype(forwarded_request)>(forwarded_request));
      });
}

template <wh::core::resume_mode Resume, typename impl_t, typename request_t, typename scheduler_t>
  requires async_prompt_handler<impl_t>
[[nodiscard]] inline auto render_sender(const impl_t &impl, request_t &&request, callback_sink sink,
                                        scheduler_t scheduler) {
  return wh::core::detail::component_async_entry<Resume>(
      std::forward<request_t>(request), std::move(sink), std::move(scheduler),
      [&impl](auto &&forwarded_request) {
        return make_render_sender(impl,
                                  std::forward<decltype(forwarded_request)>(forwarded_request));
      },
      [](const prompt_render_request &state_request) { return make_callback_state(state_request); },
      [](const callback_sink &start_sink, const callback_state &state) {
        emit_callback(start_sink, wh::callbacks::stage::start, state);
      },
      [](const callback_sink &success_sink, callback_state &state, prompt_result &status) {
        state.event.rendered_message_count = status.value().size();
        emit_callback(success_sink, wh::callbacks::stage::end, state);
      },
      [](const callback_sink &error_sink, callback_state &state, prompt_result &) {
        emit_callback(error_sink, wh::callbacks::stage::error, state);
      });
}

template <typename impl_t, typename request_t>
  requires async_prompt_handler<impl_t>
[[nodiscard]] inline auto render_sender(const impl_t &impl, request_t &&request,
                                        callback_sink sink) {
  return render_sender<wh::core::resume_mode::unchanged>(impl, std::forward<request_t>(request),
                                                         std::move(sink),
                                                         wh::core::detail::resume_passthrough);
}

} // namespace detail

template <detail::chat_template_impl impl_t,
          wh::core::resume_mode Resume = wh::core::resume_mode::unchanged>
class chat_template {
public:
  explicit chat_template(const impl_t &impl)
    requires std::copy_constructible<impl_t>
      : impl_(impl) {}

  explicit chat_template(impl_t &&impl) noexcept(std::is_nothrow_move_constructible_v<impl_t>)
      : impl_(std::move(impl)) {}

  chat_template(const chat_template &) = default;
  chat_template(chat_template &&) noexcept = default;
  auto operator=(const chat_template &) -> chat_template & = default;
  auto operator=(chat_template &&) noexcept -> chat_template & = default;
  ~chat_template() = default;

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"Prompt", wh::core::component_kind::prompt};
  }

  [[nodiscard]] auto render(const prompt_render_request &request,
                            wh::core::run_context &callback_context) const -> detail::prompt_result
    requires detail::sync_prompt_handler<impl_t>
  {
    return render_sync_impl(request, detail::borrow_callback_sink(callback_context));
  }

  [[nodiscard]] auto render(prompt_render_request &&request,
                            wh::core::run_context &callback_context) const -> detail::prompt_result
    requires detail::sync_prompt_handler<impl_t>
  {
    return render_sync_impl(std::move(request), detail::borrow_callback_sink(callback_context));
  }

  [[nodiscard]] auto async_render(const prompt_render_request &request,
                                  wh::core::run_context &callback_context) const -> auto
    requires detail::async_prompt_handler<impl_t>
  {
    return render_async_impl(request, detail::make_callback_sink(callback_context));
  }

  [[nodiscard]] auto async_render(prompt_render_request &&request,
                                  wh::core::run_context &callback_context) const -> auto
    requires detail::async_prompt_handler<impl_t>
  {
    return render_async_impl(std::move(request), detail::make_callback_sink(callback_context));
  }

  [[nodiscard]] auto impl() const noexcept -> const impl_t & { return impl_; }
  [[nodiscard]] auto impl() noexcept -> impl_t & { return impl_; }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, prompt_render_request> &&
             detail::sync_prompt_handler<impl_t>
  [[nodiscard]] auto render_sync_impl(request_t &&request, detail::callback_sink sink) const
      -> detail::prompt_result {
    sink = wh::callbacks::filter_callback_sink(std::move(sink), request.options);
    auto state = detail::make_callback_state(request);
    detail::emit_callback(sink, wh::callbacks::stage::start, state);
    auto output =
        detail::run_sync_prompt_impl(impl_, std::forward<request_t>(request), state.event);
    if (output.has_error()) {
      detail::emit_callback(sink, wh::callbacks::stage::error, state);
      return output;
    }
    state.event.rendered_message_count = output.value().size();
    detail::emit_callback(sink, wh::callbacks::stage::end, state);
    return output;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, prompt_render_request> &&
             detail::async_prompt_handler<impl_t>
  [[nodiscard]] auto render_async_impl(request_t &&request, detail::callback_sink sink) const
      -> auto {
    return wh::core::detail::defer_resume_sender<Resume>(
        [this, request = prompt_render_request{std::forward<request_t>(request)},
         sink = std::move(sink)](auto scheduler) mutable {
          return detail::render_sender<Resume>(impl_, std::move(request), std::move(sink),
                                               std::move(scheduler));
        });
  }

  wh_no_unique_address impl_t impl_;
};

template <typename impl_t> chat_template(impl_t &&) -> chat_template<std::remove_cvref_t<impl_t>>;

template <typename template_t>
concept chat_template_like =
    wh::core::component_descriptor_provider<template_t> &&
    requires(const template_t &tmpl, const prompt_render_request &request,
             prompt_render_request &&movable_request, wh::core::run_context &context) {
      {
        tmpl.render(request, context)
      } -> std::same_as<wh::core::result<std::vector<wh::schema::message>>>;
      {
        tmpl.render(std::move(movable_request), context)
      } -> std::same_as<wh::core::result<std::vector<wh::schema::message>>>;
    };

} // namespace wh::prompt
