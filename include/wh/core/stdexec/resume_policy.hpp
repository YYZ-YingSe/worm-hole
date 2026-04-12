// Defines compile-time resume policy helpers for component async wrappers.
#pragma once

#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/defer_sender.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::core {

enum class resume_mode : std::uint8_t {
  unchanged = 0U,
  restore,
};

} // namespace wh::core

namespace wh::core::detail {

struct resume_passthrough_t {};

inline constexpr resume_passthrough_t resume_passthrough{};

template <wh::core::resume_mode Mode, stdexec::sender sender_t>
[[nodiscard]] constexpr auto resume_if(sender_t &&sender,
                                       resume_passthrough_t) {
  static_assert(Mode == wh::core::resume_mode::unchanged);
  return std::forward<sender_t>(sender);
}

template <wh::core::resume_mode Mode, stdexec::sender sender_t,
          stdexec::scheduler scheduler_t>
[[nodiscard]] constexpr auto resume_if(sender_t &&sender,
                                       scheduler_t scheduler) {
  if constexpr (Mode == wh::core::resume_mode::restore) {
    return wh::core::resume_on(std::forward<sender_t>(sender),
                               std::move(scheduler));
  } else {
    return std::forward<sender_t>(sender);
  }
}

template <wh::core::resume_mode Mode, typename factory_t>
[[nodiscard]] constexpr auto defer_resume_sender(factory_t &&factory) {
  using stored_factory_t = std::remove_cvref_t<factory_t>;

  if constexpr (Mode == wh::core::resume_mode::restore) {
    return wh::core::read_resume_scheduler(
        [factory = stored_factory_t{std::forward<factory_t>(factory)}](
            auto scheduler) mutable {
          return std::invoke(factory, std::move(scheduler));
        });
  } else {
    return defer_sender([factory = stored_factory_t{
                             std::forward<factory_t>(factory)}]() mutable {
      return std::invoke(factory, resume_passthrough);
    });
  }
}

} // namespace wh::core::detail
