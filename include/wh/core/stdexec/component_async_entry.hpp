// Defines the shared async component wrapper skeleton for sender-first
// component entries with callback lifecycle emission.
#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/stdexec/inspect_result_sender.hpp"
#include "wh/core/stdexec/resume_policy.hpp"

namespace wh::core::detail {

template <typename request_t>
concept callback_filterable_request =
    requires(const std::remove_cvref_t<request_t> &request, wh::callbacks::callback_sink sink) {
      {
        wh::callbacks::filter_callback_sink(std::move(sink), request.options)
      } -> std::same_as<wh::callbacks::callback_sink>;
    };

template <wh::core::resume_mode Resume, typename request_t, typename scheduler_t,
          typename make_sender_t, typename make_state_t, typename on_start_t, typename on_success_t,
          typename on_error_t>
  requires callback_filterable_request<request_t>
[[nodiscard]] inline auto
component_async_entry(request_t &&request, wh::callbacks::callback_sink sink, scheduler_t scheduler,
                      make_sender_t &&make_sender, make_state_t &&make_state, on_start_t &&on_start,
                      on_success_t &&on_success, on_error_t &&on_error) {
  using request_value_t = std::remove_cvref_t<request_t>;
  using state_t =
      std::remove_cvref_t<std::invoke_result_t<make_state_t &, const request_value_t &>>;
  using success_t = std::remove_cvref_t<on_success_t>;
  using error_t = std::remove_cvref_t<on_error_t>;

  sink = wh::callbacks::filter_callback_sink(std::move(sink), request.options);
  auto state = std::optional<state_t>{};
  if (sink.has_value()) {
    state.emplace(std::invoke(make_state, static_cast<const request_value_t &>(request)));
    std::invoke(on_start, sink, *state);
  }

  return wh::core::detail::inspect_result_sender(
      wh::core::detail::resume_if<Resume>(
          std::invoke(std::forward<make_sender_t>(make_sender), std::forward<request_t>(request)),
          std::move(scheduler)),
      [sink = std::move(sink), state = std::move(state),
       on_success = success_t{std::forward<on_success_t>(on_success)},
       on_error = error_t{std::forward<on_error_t>(on_error)}](auto &status) mutable {
        if (!state.has_value()) {
          return;
        }
        if (status.has_error()) {
          std::invoke(on_error, sink, *state, status);
          return;
        }
        std::invoke(on_success, sink, *state, status);
      });
}

} // namespace wh::core::detail
