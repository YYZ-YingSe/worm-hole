// Defines callback manager aliases and config helpers.
#pragma once

#include <concepts>
#include <string>
#include <utility>

#include "wh/callbacks/interface.hpp"
#include "wh/internal/callbacks.hpp"

namespace wh::callbacks {

/// Callback manager used to register and dispatch callbacks.
using manager = wh::internal::callback_manager;
/// One callback stage registration entry.
using stage_registration = manager::stage_registration;
/// Callback stage registration list.
using registration_list = manager::registration_list;
/// Shared callback registration snapshot.
using shared_registration_list = manager::shared_registration_list;

template <TimingChecker timing_checker_t, typename name_t = std::string>
  requires std::constructible_from<std::string, name_t &&>
/// Creates callback config from timing checker and optional debug name.
[[nodiscard]] inline auto
make_callback_config(timing_checker_t &&checker, name_t &&name = {})
    -> callback_config {
  return wh::internal::make_callback_config(
      std::forward<timing_checker_t>(checker),
      std::forward<name_t>(name));
}

template <typename... config_t>
/// Merges multiple callback configs into one effective config.
[[nodiscard]] inline auto merge_callback_config(config_t &&...configs)
    -> callback_config {
  return wh::internal::merge_callback_config(
      std::forward<config_t>(configs)...);
}

} // namespace wh::callbacks
