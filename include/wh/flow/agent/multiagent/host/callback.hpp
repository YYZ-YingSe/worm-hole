// Defines callbacks emitted by the multi-agent host flow.
#pragma once

#include <string_view>

#include "wh/core/function.hpp"

namespace wh::flow::agent::multiagent::host {

/// Optional callbacks emitted by the host flow shell.
struct host_callbacks {
  /// Called when the host route hands off to one specialist.
  wh::core::callback_function<void(std::string_view, std::string_view) const>
      on_handoff{nullptr};
};

} // namespace wh::flow::agent::multiagent::host
