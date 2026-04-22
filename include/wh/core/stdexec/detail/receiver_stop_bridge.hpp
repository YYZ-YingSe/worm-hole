// Defines one reusable stop-token forwarding bridge for erased sender edges.
#pragma once

#include <atomic>
#include <concepts>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::core::detail {

template <typename receiver_t> class receiver_stop_bridge {
public:
  using receiver_type = std::remove_cvref_t<receiver_t>;
  using receiver_env_t =
      std::remove_cvref_t<decltype(stdexec::get_env(std::declval<const receiver_type &>()))>;
  using stop_token_env_t = stdexec::prop<stdexec::get_stop_token_t, stdexec::inplace_stop_token>;
  using stop_env_t =
      decltype(stdexec::__env::__join(std::declval<stop_token_env_t>(),
                                      std::declval<const receiver_env_t &>()));
  using outer_stop_token_t = stdexec::stop_token_of_t<receiver_env_t>;

  struct stop_callback {
    receiver_stop_bridge *bridge{nullptr};

    auto operator()() const noexcept -> void {
      bridge->stop_requested_.store(true, std::memory_order_release);
      bridge->stop_source_.request_stop();
    }
  };

  using outer_stop_callback_t = stdexec::stop_callback_for_t<outer_stop_token_t, stop_callback>;

  explicit receiver_stop_bridge(receiver_type &receiver)
      : receiver_(std::addressof(receiver)), env_(stdexec::get_env(*receiver_)) {
    auto stop_token = stdexec::get_stop_token(env_);
    if (stop_token.stop_requested()) {
      stop_requested_.store(true, std::memory_order_release);
      stop_source_.request_stop();
      return;
    }
    if constexpr (!stdexec::unstoppable_token<outer_stop_token_t>) {
      stop_callback_.emplace(stop_token, stop_callback{this});
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
        stop_source_.request_stop();
      }
    }
  }

  receiver_stop_bridge(const receiver_stop_bridge &) = delete;
  auto operator=(const receiver_stop_bridge &) -> receiver_stop_bridge & = delete;

  [[nodiscard]] auto env() const noexcept -> stop_env_t {
    return stdexec::__env::__join(
        stop_token_env_t{stdexec::get_stop_token, stop_source_.get_token()}, env_);
  }

  [[nodiscard]] auto stop_requested() const noexcept -> bool {
    return stop_requested_.load(std::memory_order_acquire);
  }

  template <typename... value_ts> auto set_value(value_ts &&...values) noexcept -> void {
    if (stop_requested()) {
      set_stopped();
      return;
    }
    if (!try_complete()) {
      return;
    }
    stdexec::set_value(std::move(*receiver_), std::forward<value_ts>(values)...);
  }

  template <typename error_t> auto set_error(error_t &&error) noexcept -> void {
    if (stop_requested()) {
      set_stopped();
      return;
    }
    if (!try_complete()) {
      return;
    }
    stdexec::set_error(std::move(*receiver_), std::forward<error_t>(error));
  }

  auto set_stopped() noexcept -> void {
    if (!try_complete()) {
      return;
    }
    stdexec::set_stopped(std::move(*receiver_));
  }

private:
  [[nodiscard]] auto try_complete() noexcept -> bool {
    auto expected = false;
    return completed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
  }

  receiver_type *receiver_{nullptr};
  receiver_env_t env_;
  stdexec::inplace_stop_source stop_source_{};
  std::optional<outer_stop_callback_t> stop_callback_{};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> completed_{false};
};

} // namespace wh::core::detail
