// Defines the public authored-shell to executable-agent conversion entrypoints.
// Callers freeze shells before invoking these adapters so the public phase path
// stays freeze() -> into_agent() -> lower().
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

inline auto react::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_react_agent(std::move(*this));
}

inline auto plan_execute::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_plan_execute_agent(std::move(*this));
}

inline auto self_refine::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_self_refine_agent(std::move(*this));
}

inline auto reviewer_executor::into_agent() &&
    -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_reviewer_executor_agent(std::move(*this));
}

inline auto reflexion::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_reflexion_agent(std::move(*this));
}

inline auto supervisor::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_supervisor_agent(std::move(*this));
}

inline auto swarm::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_swarm_agent(std::move(*this));
}

inline auto research::into_agent() && -> wh::core::result<wh::agent::agent> {
  return wh::adk::bind_research_agent(std::move(*this));
}

} // namespace wh::agent
