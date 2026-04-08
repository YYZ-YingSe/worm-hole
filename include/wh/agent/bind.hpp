// Defines public authored-shell to executable-agent bindings without exposing
// adk lowering details in each authored shell header.
#pragma once

#include <utility>

#include "wh/adk/bind.hpp"
#include "wh/agent/agent.hpp"
#include "wh/agent/chat.hpp"
#include "wh/agent/plan_execute.hpp"
#include "wh/agent/react.hpp"
#include "wh/agent/reflexion.hpp"
#include "wh/agent/reviewer_executor.hpp"
#include "wh/agent/research.hpp"
#include "wh/agent/self_refine.hpp"
#include "wh/agent/swarm.hpp"
#include "wh/agent/supervisor.hpp"

namespace wh::agent {

inline auto chat::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_chat_agent(std::move(*this));
}

/// Wraps one authored chat shell into the common executable agent surface.
[[nodiscard]] inline auto make_agent(chat authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

inline auto react::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_react_agent(std::move(*this));
}

/// Wraps one authored ReAct shell into the common executable agent surface.
[[nodiscard]] inline auto make_agent(react authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

inline auto plan_execute::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_plan_execute_agent(std::move(*this));
}

/// Wraps one authored plan-execute shell into the common executable agent
/// surface.
[[nodiscard]] inline auto make_agent(plan_execute authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

inline auto self_refine::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_self_refine_agent(std::move(*this));
}

/// Wraps one authored self-refine shell into the common executable agent
/// surface.
[[nodiscard]] inline auto make_agent(self_refine authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

inline auto reviewer_executor::into_agent() &&
    -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_reviewer_executor_agent(std::move(*this));
}

/// Wraps one authored reviewer-executor shell into the common executable
/// agent surface.
[[nodiscard]] inline auto make_agent(reviewer_executor authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

inline auto reflexion::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_reflexion_agent(std::move(*this));
}

/// Wraps one authored reflexion shell into the common executable agent
/// surface.
[[nodiscard]] inline auto make_agent(reflexion authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

inline auto supervisor::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_supervisor_agent(std::move(*this));
}

/// Wraps one authored supervisor shell into the common executable agent
/// surface.
[[nodiscard]] inline auto make_agent(supervisor authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

inline auto swarm::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_swarm_agent(std::move(*this));
}

/// Wraps one authored swarm shell into the common executable agent surface.
[[nodiscard]] inline auto make_agent(swarm authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

inline auto research::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_research_agent(std::move(*this));
}

/// Wraps one authored research shell into the common executable agent surface.
[[nodiscard]] inline auto make_agent(research authored)
    -> wh::core::result<wh::agent::agent> {
  return std::move(authored).into_agent();
}

} // namespace wh::agent
