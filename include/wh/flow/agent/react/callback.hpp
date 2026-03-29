// Defines flow-level ReAct callbacks scoped to exported node names.
#pragma once

#include <string_view>

#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/schema/message.hpp"

namespace wh::flow::agent::react {

/// Optional callbacks emitted by the ReAct flow shell.
struct react_callbacks {
  /// Called for each visible message routed through the exported flow.
  wh::core::callback_function<void(std::string_view, const wh::schema::message &) const>
      on_message{nullptr};
  /// Called when the exported flow surfaces one terminal error item.
  wh::core::callback_function<void(std::string_view, wh::core::error_code) const>
      on_error{nullptr};
};

} // namespace wh::flow::agent::react
