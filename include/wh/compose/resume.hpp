#pragma once

#include <any>
#include <string>
#include <type_traits>
#include <utility>

#include "wh/core/resume_state.hpp"

namespace wh::compose {

inline auto merge_resume_state(wh::core::resume_state &target,
                               const wh::core::resume_state &delta)
    -> wh::core::result<void> {
  return target.merge(delta);
}

inline auto merge_resume_state(wh::core::resume_state &target,
                               wh::core::resume_state &&delta)
    -> wh::core::result<void> {
  return target.merge(std::move(delta));
}

inline auto add_resume_target(wh::core::resume_state &state,
                              std::string interrupt_id,
                              wh::core::address location, std::any data)
    -> wh::core::result<void> {
  return state.upsert(std::move(interrupt_id), std::move(location),
                      std::move(data));
}

template <typename value_t>
inline auto add_resume_target(wh::core::resume_state &state,
                              std::string interrupt_id,
                              wh::core::address location, value_t &&data)
    -> wh::core::result<void> {
  using stored_t = std::remove_cvref_t<value_t>;
  if constexpr (std::same_as<stored_t, std::any>) {
    return add_resume_target(state, std::move(interrupt_id),
                             std::move(location), std::forward<value_t>(data));
  } else {
    return state.upsert(std::move(interrupt_id), std::move(location),
                        std::forward<value_t>(data));
  }
}

template <typename value_t>
[[nodiscard]] inline auto
consume_resume_data(wh::core::resume_state &state,
                    const std::string_view interrupt_id)
    -> wh::core::result<value_t> {
  return state.consume<value_t>(interrupt_id);
}

[[nodiscard]] inline auto
next_resume_points(const wh::core::resume_state &state,
                   const wh::core::address &location)
    -> std::vector<std::string> {
  return state.next_resume_points(location);
}

} // namespace wh::compose
