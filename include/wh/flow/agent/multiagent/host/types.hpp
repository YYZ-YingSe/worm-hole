// Defines public multi-agent host flow data contracts.
#pragma once

#include <string>
#include <vector>

#include "wh/flow/agent/utils.hpp"
#include "wh/schema/message.hpp"

namespace wh::flow::agent::multiagent::host {

/// Source kind used by one specialist binding.
enum class specialist_kind {
  /// Specialist is backed by a chat model.
  model = 0U,
  /// Specialist is backed by a single-message callable.
  value,
  /// Specialist is backed by a streaming callable.
  stream,
};

/// One specialist response collected during a host run.
struct specialist_result {
  /// Stable specialist name.
  std::string specialist_name{};
  /// Binding kind used for this specialist.
  specialist_kind kind{specialist_kind::value};
  /// Final specialist message.
  wh::schema::message message{};
};

/// Immutable execution report emitted after one host run.
struct host_report {
  /// True when the host answered directly without handing off.
  bool direct_answer{false};
  /// Ordered selected specialist names after de-duplication and stabilization.
  std::vector<std::string> selected_specialists{};
  /// Collected specialist outputs, when any.
  std::vector<specialist_result> specialist_results{};
};

/// Final result bundle returned after one host run.
struct host_result {
  /// Final output message returned by the flow.
  wh::schema::message message{};
  /// Immutable execution report for diagnostics and tests.
  host_report report{};
};

} // namespace wh::flow::agent::multiagent::host
