// Defines interrupt/resume runtime helpers extracted from graph execution core.
#pragma once

#include <algorithm>
#include <any>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::interrupt_runtime {

struct graph_interrupt_state {
  graph_external_interrupt_policy policy{};
  graph_external_interrupt_policy_latch policy_latch{};
  bool wait_mode_active{false};
  bool freeze_requested{false};
  bool freeze_external{false};
  std::optional<std::chrono::steady_clock::time_point> deadline{};
};

struct external_interrupt_boundary_state {
  bool wait_mode_active{false};
  std::optional<std::chrono::steady_clock::time_point> deadline{};
};

[[nodiscard]] inline auto evaluate_hook(wh::core::run_context &context,
                                        const graph_interrupt_node_hook &hook,
                                        const std::string_view node_key, const graph_value &payload)
    -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
  if (!hook) {
    return std::optional<wh::core::interrupt_signal>{};
  }
  return hook(node_key, payload, context);
}

[[nodiscard]] inline auto
freeze_external_policy_from_latch(graph_external_interrupt_policy_latch &latch,
                                  const graph_external_interrupt_policy &policy)
    -> wh::core::result<const graph_external_interrupt_policy *> {
  auto &frozen = freeze_external_interrupt_policy(latch, policy);
  return std::addressof(frozen);
}

[[nodiscard]] inline auto
resolve_external_resolution_kind(const graph_external_interrupt_policy &policy) noexcept
    -> graph_external_interrupt_resolution_kind {
  if (policy.mode == graph_interrupt_timeout_mode::immediate_rerun) {
    return graph_external_interrupt_resolution_kind::immediate_rerun;
  }
  if (policy.timeout.has_value() && policy.timeout.value() == std::chrono::milliseconds{0}) {
    return graph_external_interrupt_resolution_kind::immediate_rerun;
  }
  return graph_external_interrupt_resolution_kind::wait_inflight;
}

inline auto
apply_runtime_resume_controls(wh::core::run_context &context,
                              const wh::compose::detail::runtime_state::invoke_config &config)
    -> wh::core::result<void> {
  const std::vector<wh::core::interrupt_context> *root_contexts_ptr = nullptr;
  std::vector<wh::core::interrupt_context> synthesized_root_contexts{};
  if (!config.resume_contexts.empty()) {
    root_contexts_ptr = &config.resume_contexts;
  } else if (context.interrupt_info.has_value()) {
    auto owned_interrupt = wh::core::into_owned(*context.interrupt_info);
    if (owned_interrupt.has_error()) {
      return wh::core::result<void>::failure(owned_interrupt.error());
    }
    synthesized_root_contexts.push_back(std::move(owned_interrupt).value());
    root_contexts_ptr = &synthesized_root_contexts;
  }
  const auto subgraph_span = std::span<const wh::core::interrupt_signal>{
      config.subgraph_interrupt_signals.data(), config.subgraph_interrupt_signals.size()};
  const auto root_contexts_span =
      root_contexts_ptr != nullptr
          ? std::span<const wh::core::interrupt_context>{root_contexts_ptr->data(),
                                                         root_contexts_ptr->size()}
          : std::span<const wh::core::interrupt_context>{};
  auto owned_contexts = merge_interrupt_sources(root_contexts_span, subgraph_span);
  if (owned_contexts.has_error()) {
    return wh::core::result<void>::failure(owned_contexts.error());
  }
  if (owned_contexts->empty() && context.interrupt_info.has_value()) {
    owned_contexts = fallback_to_single_interrupt_when_empty(std::move(owned_contexts).value(),
                                                             context.interrupt_info->location,
                                                             context.interrupt_info->state);
  }

  const bool has_runtime_resume_patch =
      config.resume_decision.has_value() || !config.batch_resume_items.empty();
  if (has_runtime_resume_patch && !context.resume_info.has_value()) {
    context.resume_info.emplace();
  }
  if (config.resume_decision.has_value()) {
    if (!context.resume_info.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto match =
        std::ranges::find_if(owned_contexts.value(), [&](const wh::core::interrupt_context &item) {
          return item.interrupt_id == config.resume_decision->interrupt_context_id;
        });
    if (match == owned_contexts->end()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto applied =
        apply_resume_decision(context.resume_info.value(), *match, *config.resume_decision);
    if (applied.has_error()) {
      return wh::core::result<void>::failure(applied.error());
    }
  }

  if (!config.batch_resume_items.empty()) {
    if (!context.resume_info.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto status =
        apply_resume_batch(context.resume_info.value(),
                           std::span<const wh::core::interrupt_context>{owned_contexts->data(),
                                                                        owned_contexts->size()},
                           std::span<const resume_batch_item>{config.batch_resume_items.data(),
                                                              config.batch_resume_items.size()});
    if (status.has_error()) {
      return wh::core::result<void>::failure(status.error());
    }
  }
  const bool should_reinterrupt = config.reinterrupt_unmatched;
  if (should_reinterrupt && !owned_contexts->empty() && context.resume_info.has_value()) {
    const auto unmatched = collect_reinterrupts(
        context.resume_info.value(), std::span<const wh::core::interrupt_context>{
                                         owned_contexts->data(), owned_contexts->size()});
    if (unmatched.has_error()) {
      return wh::core::result<void>::failure(unmatched.error());
    }
    if (!unmatched->empty()) {
      auto interrupt_context = wh::compose::to_interrupt_context(unmatched->front());
      if (interrupt_context.has_error()) {
        return wh::core::result<void>::failure(interrupt_context.error());
      }
      context.interrupt_info = std::move(interrupt_context).value();
    }
  }
  return {};
}

inline auto apply_resume_data_state_overrides(wh::core::run_context &context,
                                              graph_state_table &state_table)
    -> wh::core::result<void> {
  if (!context.resume_info.has_value()) {
    return {};
  }
  for (const auto &interrupt_id : context.resume_info->interrupt_ids(true)) {
    auto node_state_ref = context.resume_info->peek<graph_node_state>(interrupt_id);
    if (node_state_ref.has_error()) {
      if (node_state_ref.error() == wh::core::errc::type_mismatch ||
          node_state_ref.error() == wh::core::errc::not_found) {
        continue;
      }
      return wh::core::result<void>::failure(node_state_ref.error());
    }
    const auto &node_state = node_state_ref.value().get();
    auto updated = state_table.update(node_state.node_id, node_state.lifecycle, node_state.attempts,
                                      node_state.last_error);
    if (updated.has_error() && updated.error() != wh::core::errc::not_found) {
      return wh::core::result<void>::failure(updated.error());
    }
  }
  return {};
}

template <typename persist_fn_t>
[[nodiscard]] inline auto
handle_external_boundary(wh::compose::detail::runtime_state::invoke_outputs &outputs,
                         graph_external_interrupt_policy_latch &latch,
                         const graph_external_interrupt_policy &policy,
                         external_interrupt_boundary_state &state, persist_fn_t &&persist_interrupt)
    -> wh::core::result<bool> {
  const auto *frozen_policy = freeze_external_policy_from_latch(latch, policy).value();
  const auto resolution = resolve_external_resolution_kind(*frozen_policy);
  outputs.external_interrupt_resolution = resolution;
  if (resolution == graph_external_interrupt_resolution_kind::immediate_rerun) {
    auto persisted = std::forward<persist_fn_t>(persist_interrupt)(true);
    if (persisted.has_error()) {
      return wh::core::result<bool>::failure(persisted.error());
    }
    return true;
  }
  if (!state.wait_mode_active) {
    state.wait_mode_active = true;
    if (frozen_policy->timeout.has_value()) {
      state.deadline = std::chrono::steady_clock::now() + frozen_policy->timeout.value();
    } else {
      state.deadline.reset();
    }
  }
  if (state.deadline.has_value() && std::chrono::steady_clock::now() >= *state.deadline) {
    auto persisted = std::forward<persist_fn_t>(persist_interrupt)(true);
    if (persisted.has_error()) {
      return wh::core::result<bool>::failure(persisted.error());
    }
    return true;
  }
  return false;
}

template <typename persist_fn_t>
[[nodiscard]] inline auto handle_external_boundary(
    wh::core::run_context &context, wh::compose::detail::runtime_state::invoke_outputs &outputs,
    graph_external_interrupt_policy_latch &latch, const graph_external_interrupt_policy &policy,
    external_interrupt_boundary_state &state, persist_fn_t &&persist_interrupt)
    -> wh::core::result<bool> {
  if (!context.interrupt_info.has_value() || context.interrupt_info->interrupt_id.empty()) {
    return false;
  }
  return handle_external_boundary(outputs, latch, policy, state,
                                  std::forward<persist_fn_t>(persist_interrupt));
}

} // namespace wh::compose::detail::interrupt_runtime
