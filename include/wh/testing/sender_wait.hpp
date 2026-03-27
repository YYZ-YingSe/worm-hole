#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

namespace wh::testing {

template <typename scheduler_t, typename stop_token_t = stdexec::never_stop_token>
struct scheduler_env {
  scheduler_t scheduler;
  stop_token_t stop_token{};

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
      -> scheduler_t {
    return scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
      -> scheduler_t {
    return scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
      -> stop_token_t {
    return stop_token;
  }
};

struct no_scheduler_env {
  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
      -> stdexec::never_stop_token {
    return {};
  }
};

template <typename scheduler_t, typename stop_token_t = stdexec::never_stop_token>
[[nodiscard]] auto make_scheduler_env(scheduler_t scheduler,
                                      stop_token_t stop_token = {})
    -> scheduler_env<std::remove_cvref_t<scheduler_t>, stop_token_t> {
  return {std::move(scheduler), std::move(stop_token)};
}

enum class sender_terminal_kind : std::uint8_t {
  none = 0U,
  value = 1U,
  error = 2U,
  stopped = 3U,
};

struct sender_completion_state {
  std::binary_semaphore ready{0};
  sender_terminal_kind terminal{sender_terminal_kind::none};
};

template <typename value_t> struct sender_value_state {
  std::binary_semaphore ready{0};
  sender_terminal_kind terminal{sender_terminal_kind::none};
  std::optional<value_t> value{};
};

template <typename env_t = no_scheduler_env> struct sender_completion_receiver {
  using receiver_concept = stdexec::receiver_t;

  sender_completion_state *state{nullptr};
  env_t env{};

  template <typename... values_t> auto set_value(values_t &&...) noexcept -> void {
    state->terminal = sender_terminal_kind::value;
    state->ready.release();
  }

  template <typename error_t> auto set_error(error_t &&) noexcept -> void {
    state->terminal = sender_terminal_kind::error;
    state->ready.release();
  }

  auto set_stopped() noexcept -> void {
    state->terminal = sender_terminal_kind::stopped;
    state->ready.release();
  }

  [[nodiscard]] auto get_env() const noexcept -> env_t { return env; }
};

template <typename value_t, typename env_t = no_scheduler_env>
struct sender_value_receiver {
  using receiver_concept = stdexec::receiver_t;

  sender_value_state<value_t> *state{nullptr};
  env_t env{};

  template <typename received_t> auto set_value(received_t &&value) noexcept -> void {
    state->value.emplace(std::forward<received_t>(value));
    state->terminal = sender_terminal_kind::value;
    state->ready.release();
  }

  template <typename error_t> auto set_error(error_t &&) noexcept -> void {
    state->terminal = sender_terminal_kind::error;
    state->ready.release();
  }

  auto set_stopped() noexcept -> void {
    state->terminal = sender_terminal_kind::stopped;
    state->ready.release();
  }

  [[nodiscard]] auto get_env() const noexcept -> env_t { return env; }
};

template <typename operation_t> auto start_operation(operation_t &operation) -> void {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  stdexec::start(operation);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

template <typename sender_t, typename rep_t, typename period_t,
          typename env_t = no_scheduler_env>
[[nodiscard]] auto wait_for_completion(
    sender_t &&sender, const std::chrono::duration<rep_t, period_t> timeout,
    env_t env = {}) -> bool {
  sender_completion_state state{};
  auto operation = stdexec::connect(
      std::forward<sender_t>(sender),
      sender_completion_receiver<env_t>{&state, std::move(env)});
  start_operation(operation);
  return state.ready.try_acquire_for(timeout) &&
         state.terminal == sender_terminal_kind::value;
}

template <typename value_t, typename sender_t, typename rep_t, typename period_t,
          typename env_t = no_scheduler_env>
[[nodiscard]] auto wait_for_value(sender_t &&sender, value_t &value,
                                  const std::chrono::duration<rep_t, period_t> timeout,
                                  env_t env = {}) -> bool {
  sender_value_state<value_t> state{};
  auto operation = stdexec::connect(
      std::forward<sender_t>(sender),
      sender_value_receiver<value_t, env_t>{&state, std::move(env)});
  start_operation(operation);
  if (!state.ready.try_acquire_for(timeout) ||
      state.terminal != sender_terminal_kind::value ||
      !state.value.has_value()) {
    return false;
  }
  value = std::move(*state.value);
  return true;
}

} // namespace wh::testing
