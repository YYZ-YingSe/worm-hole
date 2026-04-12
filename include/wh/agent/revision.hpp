// Defines shared typed contracts for agent patterns that iterate between one
// draft role and one review role.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "wh/agent/agent.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/schema/message.hpp"

namespace wh::agent {

/// Read-only authored context shared by draft/review/memory request builders in
/// revision-style agent shells.
struct revision_context {
  /// Original input conversation passed into the outer authored shell.
  std::span<const wh::schema::message> input_messages{};
  /// Earlier accepted-or-replaced draft outputs, excluding `current_draft`.
  std::span<const wh::agent::agent_output> draft_history{};
  /// Current draft output when one draft turn has already completed.
  const wh::agent::agent_output *current_draft{nullptr};
  /// Earlier review outputs, excluding `current_review`.
  std::span<const wh::agent::agent_output> review_history{};
  /// Current review output when one review turn has already completed.
  const wh::agent::agent_output *current_review{nullptr};
  /// Remaining revision iterations before the loop must stop.
  std::size_t remaining_iterations{0U};
};

/// One normalized review outcome emitted by reviewer-style roles.
enum class review_decision_kind {
  /// Accept the current draft and terminate the revision loop.
  accept = 0U,
  /// Revise the current draft and continue the loop.
  revise,
};

/// Parsed branch decision produced by one review role.
struct review_decision {
  /// Selected branch after the current review turn completes.
  review_decision_kind kind{review_decision_kind::accept};
};

/// Builds the next role request from the current revision context.
using revision_request_builder = wh::core::callback_function<
    wh::core::result<std::vector<wh::schema::message>>(
        const revision_context &, wh::core::run_context &) const>;

/// Normalizes one review-role output into a branch decision.
using review_decision_reader = wh::agent::output_reader<review_decision>;

} // namespace wh::agent
