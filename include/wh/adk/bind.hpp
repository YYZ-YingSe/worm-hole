// Defines the stable ADK bind facade that lowers authored agent shells onto
// executable agents without exposing detail names to authored surfaces.
#pragma once

#include <utility>

#include "wh/adk/detail/bind_impl.hpp"

namespace wh::adk {

[[nodiscard]] inline auto bind_chat_agent(wh::agent::chat authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_chat_agent(std::move(authored));
}

[[nodiscard]] inline auto bind_react_agent(wh::agent::react authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_react_agent(std::move(authored));
}

[[nodiscard]] inline auto bind_plan_execute_agent(wh::agent::plan_execute authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_plan_execute_agent(std::move(authored));
}

[[nodiscard]] inline auto bind_self_refine_agent(wh::agent::self_refine authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_self_refine_agent(std::move(authored));
}

[[nodiscard]] inline auto bind_reviewer_executor_agent(wh::agent::reviewer_executor authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_reviewer_executor_agent(std::move(authored));
}

[[nodiscard]] inline auto bind_reflexion_agent(wh::agent::reflexion authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_reflexion_agent(std::move(authored));
}

[[nodiscard]] inline auto bind_supervisor_agent(wh::agent::supervisor authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_supervisor_agent(std::move(authored));
}

[[nodiscard]] inline auto bind_swarm_agent(wh::agent::swarm authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_swarm_agent(std::move(authored));
}

[[nodiscard]] inline auto bind_research_agent(wh::agent::research authored)
    -> wh::core::result<wh::agent::agent> {
  return detail::bind_research_agent(std::move(authored));
}

} // namespace wh::adk
