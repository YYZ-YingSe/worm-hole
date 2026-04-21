// Defines timer helper utilities for timed-scheduler delayed execution and
// timeout-based async operations.
#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include <exec/timed_scheduler.hpp>
#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::core {

/// Detects schedulers that support timed operations.
template <typename scheduler_t>
concept timed_scheduler_like = exec::timed_scheduler<remove_cvref_t<scheduler_t>>;

/// Reads current scheduler clock time.
template <timed_scheduler_like scheduler_t>
[[nodiscard]] constexpr auto scheduler_now(const scheduler_t &scheduler) {
  return exec::now(scheduler);
}

/// Builds a sender that completes after the provided relative duration.
template <timed_scheduler_like scheduler_t, typename duration_t>
[[nodiscard]] constexpr auto schedule_after(const scheduler_t &scheduler, duration_t &&duration) {
  return exec::schedule_after(scheduler, std::forward<duration_t>(duration));
}

/// Builds a sender that completes at the provided absolute deadline.
template <timed_scheduler_like scheduler_t, typename deadline_t>
[[nodiscard]] constexpr auto schedule_at(const scheduler_t &scheduler, deadline_t &&deadline) {
  return exec::schedule_at(scheduler, std::forward<deadline_t>(deadline));
}

/// Races `wait_sender` with a relative timeout; timeout returns
/// `result_t::failure(timeout)`.
template <typename result_t, timed_scheduler_like scheduler_t, stdexec::sender wait_sender_t,
          typename duration_t>
[[nodiscard]] constexpr auto timeout(const scheduler_t &scheduler, wait_sender_t &&wait_sender,
                                     duration_t &&duration) {
  return exec::when_any(
      std::forward<wait_sender_t>(wait_sender),
      schedule_after(scheduler, std::forward<duration_t>(duration)) |
          stdexec::then([]() noexcept { return result_t::failure(errc::timeout); }));
}

/// Races `wait_sender` with an absolute deadline; deadline returns
/// `result_t::failure(timeout)`.
template <typename result_t, timed_scheduler_like scheduler_t, stdexec::sender wait_sender_t,
          typename deadline_t>
[[nodiscard]] constexpr auto timeout_at(const scheduler_t &scheduler, wait_sender_t &&wait_sender,
                                        deadline_t &&deadline) {
  return exec::when_any(
      std::forward<wait_sender_t>(wait_sender),
      schedule_at(scheduler, std::forward<deadline_t>(deadline)) |
          stdexec::then([]() noexcept { return result_t::failure(errc::timeout); }));
}

} // namespace wh::core
