#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <semaphore>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "helper/sender_env.hpp"

namespace wh::testing::helper {

enum class sender_terminal_kind : std::uint8_t {
  none = 0U,
  value = 1U,
  error = 2U,
  stopped = 3U,
};

struct sender_capture_terminal {
  std::binary_semaphore ready{0};
  sender_terminal_kind terminal{sender_terminal_kind::none};
  std::exception_ptr error{};
};

template <typename value_t = void> struct sender_capture;

template <> struct sender_capture<void> : sender_capture_terminal {};

template <typename value_t> struct sender_capture : sender_capture_terminal {
  std::optional<value_t> value{};
};

template <typename value_t = void, typename env_t = no_scheduler_env>
struct sender_capture_receiver;

namespace detail {

template <typename capture_t, typename error_t>
auto capture_sender_error(capture_t &capture, error_t &&error) noexcept -> void {
  using stored_error_t = std::remove_cvref_t<error_t>;
  if constexpr (std::same_as<stored_error_t, std::exception_ptr>) {
    capture.error = std::forward<error_t>(error);
  } else if constexpr (std::copy_constructible<stored_error_t>) {
    try {
      throw std::forward<error_t>(error);
    } catch (...) {
      capture.error = std::current_exception();
    }
  }
}

} // namespace detail

template <typename env_t> struct sender_capture_receiver<void, env_t> {
  using receiver_concept = stdexec::receiver_t;

  sender_capture<void> *state{nullptr};
  env_t env{};

  template <typename... values_t> auto set_value(values_t &&...) noexcept -> void {
    state->terminal = sender_terminal_kind::value;
    state->ready.release();
  }

  template <typename error_t> auto set_error(error_t &&error) noexcept -> void {
    detail::capture_sender_error(*state, std::forward<error_t>(error));
    state->terminal = sender_terminal_kind::error;
    state->ready.release();
  }

  auto set_stopped() noexcept -> void {
    state->terminal = sender_terminal_kind::stopped;
    state->ready.release();
  }

  [[nodiscard]] auto get_env() const noexcept -> env_t { return env; }
};

template <typename value_t, typename env_t> struct sender_capture_receiver {
  using receiver_concept = stdexec::receiver_t;

  sender_capture<value_t> *state{nullptr};
  env_t env{};

  template <typename received_t> auto set_value(received_t &&value) noexcept -> void {
    state->value.emplace(std::forward<received_t>(value));
    state->terminal = sender_terminal_kind::value;
    state->ready.release();
  }

  template <typename error_t> auto set_error(error_t &&error) noexcept -> void {
    detail::capture_sender_error(*state, std::forward<error_t>(error));
    state->terminal = sender_terminal_kind::error;
    state->ready.release();
  }

  auto set_stopped() noexcept -> void {
    state->terminal = sender_terminal_kind::stopped;
    state->ready.release();
  }

  [[nodiscard]] auto get_env() const noexcept -> env_t { return env; }
};

template <typename value_t, typename sender_t, typename rep_t, typename period_t,
          typename env_t = no_scheduler_env>
[[nodiscard]] auto wait_for_value(sender_t &&sender, value_t &value,
                                  const std::chrono::duration<rep_t, period_t> timeout,
                                  env_t env = {}) -> bool {
  sender_capture<value_t> capture{};
  auto operation =
      stdexec::connect(std::forward<sender_t>(sender),
                       sender_capture_receiver<value_t, env_t>{&capture, std::move(env)});
  stdexec::start(operation);
  if (!capture.ready.try_acquire_for(timeout) || capture.terminal != sender_terminal_kind::value ||
      !capture.value.has_value()) {
    return false;
  }
  value = std::move(*capture.value);
  return true;
}

template <typename env_t>
sender_capture_receiver(sender_capture<void> *, env_t)
    -> sender_capture_receiver<void, std::remove_cvref_t<env_t>>;

template <typename value_t, typename env_t>
sender_capture_receiver(sender_capture<value_t> *, env_t)
    -> sender_capture_receiver<value_t, std::remove_cvref_t<env_t>>;

} // namespace wh::testing::helper
