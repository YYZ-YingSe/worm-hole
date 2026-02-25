#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include "wh/scheduler/scheduler_context.hpp"

namespace wh::core {

template <typename context_t>
concept has_execution_scheduler_in_context =
    requires(std::remove_cvref_t<context_t> context) {
      typename std::remove_cvref_t<context_t>::execution_scheduler_type;
      context.execution_scheduler;
    };

template <typename context_t>
inline constexpr bool has_execution_scheduler_in_context_v =
    has_execution_scheduler_in_context<context_t>;

template <typename scheduler_t>
  requires stdexec_scheduler<scheduler_t>
[[nodiscard]] constexpr auto
make_scheduler_context(scheduler_t &&scheduler)
    -> scheduler_context<std::remove_cvref_t<scheduler_t>> {
  return scheduler_context<std::remove_cvref_t<scheduler_t>>{
      std::forward<scheduler_t>(scheduler)};
}

template <typename context_t>
  requires has_execution_scheduler_in_context<context_t>
[[nodiscard]] constexpr auto
select_execution_scheduler(const context_t &context) noexcept
    -> decltype(auto) {
  return (context.execution_scheduler);
}

template <typename context_t>
[[nodiscard]] constexpr auto
is_scheduler_bound([[maybe_unused]] const context_t &context) noexcept -> bool {
  return has_execution_scheduler_in_context<context_t>;
}

} // namespace wh::core
