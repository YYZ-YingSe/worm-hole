#pragma once

#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

namespace wh::core {

struct try_schedule_t;

namespace detail {

template <typename scheduler_t>
concept try_schedule_member_callable =
    requires(scheduler_t &&scheduler) { static_cast<scheduler_t &&>(scheduler).try_schedule(); };

} // namespace detail

/// Customization point for non-blocking scheduler handoff via
/// `scheduler.try_schedule()`.
struct try_schedule_t {
  template <typename scheduler_t>
    requires detail::try_schedule_member_callable<scheduler_t>
  [[nodiscard]] constexpr auto operator()(scheduler_t &&scheduler) const -> decltype(auto) {
    return static_cast<scheduler_t &&>(scheduler).try_schedule();
  }
};

/// Reusable CPO instance for non-blocking scheduling.
inline constexpr try_schedule_t try_schedule{};

/// Scheduler concept extended with member `try_schedule()` support.
template <typename scheduler_t>
concept try_scheduler =
    stdexec::scheduler<std::remove_cvref_t<scheduler_t>> && requires(scheduler_t &&scheduler) {
      { wh::core::try_schedule(static_cast<scheduler_t &&>(scheduler)) } -> stdexec::sender;
    };

template <typename scheduler_t>
using try_schedule_result_t = decltype(wh::core::try_schedule(std::declval<scheduler_t>()));

} // namespace wh::core
