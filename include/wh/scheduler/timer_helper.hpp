#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include <exec/timed_scheduler.hpp>
#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/scheduler/context_helper.hpp"

namespace wh::core {

template <typename context_t>
concept timed_scheduler_in_context =
    has_execution_scheduler_in_context<context_t> &&
    exec::timed_scheduler<
        typename std::remove_cvref_t<context_t>::execution_scheduler_type>;

template <timed_scheduler_in_context context_t>
[[nodiscard]] constexpr auto
select_timer_scheduler(const context_t &context) noexcept -> decltype(auto) {
  return select_execution_scheduler(context);
}

template <timed_scheduler_in_context context_t>
[[nodiscard]] constexpr auto context_now(const context_t &context) {
  return exec::now(select_timer_scheduler(context));
}

template <timed_scheduler_in_context context_t, typename duration_t>
[[nodiscard]] constexpr auto schedule_after_context(const context_t &context,
                                                    duration_t &&duration) {
  return exec::schedule_after(select_timer_scheduler(context),
                              std::forward<duration_t>(duration));
}

template <timed_scheduler_in_context context_t, typename deadline_t>
[[nodiscard]] constexpr auto schedule_at_context(const context_t &context,
                                                 deadline_t &&deadline) {
  return exec::schedule_at(select_timer_scheduler(context),
                           std::forward<deadline_t>(deadline));
}

template <typename result_t, timed_scheduler_in_context context_t,
          stdexec::sender wait_sender_t, typename duration_t>
[[nodiscard]] constexpr auto timeout(const context_t &context,
                                     wait_sender_t &&wait_sender,
                                     duration_t &&duration) {
  return exec::when_any(
      std::forward<wait_sender_t>(wait_sender),
      schedule_after_context(context, std::forward<duration_t>(duration)) |
          stdexec::then(
              []() noexcept { return result_t::failure(errc::timeout); }));
}

template <typename result_t, timed_scheduler_in_context context_t,
          stdexec::sender wait_sender_t, typename deadline_t>
[[nodiscard]] constexpr auto timeout_at(const context_t &context,
                                        wait_sender_t &&wait_sender,
                                        deadline_t &&deadline) {
  return exec::when_any(
      std::forward<wait_sender_t>(wait_sender),
      schedule_at_context(context, std::forward<deadline_t>(deadline)) |
          stdexec::then(
              []() noexcept { return result_t::failure(errc::timeout); }));
}

} // namespace wh::core
