// Defines the ADK-side interrupt and resume mapping layer as one thin public
// projection of compose and core interrupt semantics.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "wh/compose/runtime/resume.hpp"
#include "wh/core/any.hpp"
#include "wh/core/resume_state.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::adk {

/// Human decision kind captured at the ADK protocol boundary.
enum class interrupt_resolution : std::uint8_t {
  /// Resume with the captured interrupt payload unchanged.
  approve = 0U,
  /// Resume with caller-edited replacement payload.
  edit,
  /// Reject resume and keep interrupt pending.
  reject,
};

/// Human or audit metadata attached to one ADK interrupt decision.
struct interrupt_audit {
  /// Stable audit identifier for trace or replay correlation.
  std::string audit_id{};
  /// Operator, agent, or user identity that made the decision.
  std::string actor{};
  /// Optional human-readable explanation for the decision.
  std::string reason{};
};

/// ADK-visible resume patch payload attached to one interrupt decision.
struct interrupt_patch {
  /// Decision kind applied to the target interrupt.
  interrupt_resolution resolution{interrupt_resolution::approve};
  /// Optional edited payload used by `edit`.
  wh::core::any payload{};
  /// Audit metadata persisted with the patch.
  interrupt_audit audit{};
};

/// Explainable ADK-visible resume-target classification for one address.
enum class interrupt_target_kind {
  /// Current address is not in the active resume subtree.
  none = 0U,
  /// Current address exactly matches one active resume target.
  exact,
  /// Current address is ancestor of one active resume target.
  descendant,
};

/// ADK-visible interrupt projection built from one core interrupt context.
struct interrupt_info {
  /// Stable interrupt identifier.
  std::string interrupt_id{};
  /// Projected ADK run path exposing only agent or tool segments.
  wh::core::address run_path{};
  /// Interrupt-local serialized state payload.
  wh::core::any state{};
  /// Layer-specific payload captured alongside interrupt state.
  wh::core::any payload{};
  /// Resume-target classification for the current path.
  interrupt_target_kind target_kind{interrupt_target_kind::none};
  /// True once the underlying interrupt context was consumed.
  bool used{false};
  /// Human-readable interrupt reason, when the source populated it.
  std::string trigger_reason{};
};

/// One explicit ADK patch batch item keyed by interrupt id.
struct interrupt_patch_item {
  /// Stable interrupt identifier targeted by this patch.
  std::string interrupt_id{};
  /// Patch payload applied to the target.
  interrupt_patch patch{};
};

namespace detail {

[[nodiscard]] inline auto to_compose_decision(
    const interrupt_resolution resolution) noexcept
    -> wh::compose::interrupt_decision_kind {
  switch (resolution) {
  case interrupt_resolution::approve:
    return wh::compose::interrupt_decision_kind::approve;
  case interrupt_resolution::edit:
    return wh::compose::interrupt_decision_kind::edit;
  case interrupt_resolution::reject:
    return wh::compose::interrupt_decision_kind::reject;
  }
  return wh::compose::interrupt_decision_kind::approve;
}

[[nodiscard]] inline auto to_compose_audit(const interrupt_audit &audit)
    -> wh::compose::interrupt_decision_audit {
  return wh::compose::interrupt_decision_audit{
      .audit_id = audit.audit_id,
      .actor = audit.actor,
      .reason = audit.reason,
  };
}

} // namespace detail

/// Projects one runtime address into the ADK-visible agent or tool path dialect.
[[nodiscard]] inline auto project_interrupt_run_path(
    const wh::core::address &location) -> wh::core::address {
  wh::core::address projected{};
  const auto segments = location.segments();
  for (std::size_t index = 0U; index < segments.size(); ++index) {
    if (segments[index] == "agent" && index + 1U < segments.size()) {
      projected = projected.append("agent").append(segments[index + 1U]);
      ++index;
      continue;
    }
    if (segments[index] == "tool" && index + 1U < segments.size()) {
      projected = projected.append("tool").append(segments[index + 1U]);
      ++index;
      if (index + 1U < segments.size()) {
        projected = projected.append(segments[index + 1U]);
        ++index;
      }
      continue;
    }
  }
  return projected;
}

/// Classifies current runtime address against one active resume state.
[[nodiscard]] inline auto classify_interrupt_target(
    const wh::core::resume_state &state,
    const wh::core::address &location) noexcept -> interrupt_target_kind {
  const auto classified =
      wh::compose::classify_resume_target_match(state, location);
  switch (classified.match_kind) {
  case wh::compose::resume_target_match_kind::exact:
    return interrupt_target_kind::exact;
  case wh::compose::resume_target_match_kind::descendant:
    return interrupt_target_kind::descendant;
  case wh::compose::resume_target_match_kind::none:
    return interrupt_target_kind::none;
  }
  return interrupt_target_kind::none;
}

/// Builds one ADK interrupt projection from the underlying core interrupt context.
[[nodiscard]] inline auto make_interrupt_info(
    const wh::core::interrupt_context &context,
    const wh::core::resume_state *resume_state = nullptr)
    -> wh::core::result<interrupt_info> {
  auto state = wh::core::into_owned(context.state);
  if (state.has_error()) {
    return wh::core::result<interrupt_info>::failure(state.error());
  }
  auto payload = wh::core::into_owned(context.layer_payload);
  if (payload.has_error()) {
    return wh::core::result<interrupt_info>::failure(payload.error());
  }
  return interrupt_info{
      .interrupt_id = context.interrupt_id,
      .run_path = project_interrupt_run_path(context.location),
      .state = std::move(state).value(),
      .payload = std::move(payload).value(),
      .target_kind =
          resume_state == nullptr
              ? interrupt_target_kind::none
              : classify_interrupt_target(*resume_state, context.location),
      .used = context.used,
      .trigger_reason = context.trigger_reason,
  };
}

/// Builds one ADK interrupt projection from run-context state when available.
[[nodiscard]] inline auto current_interrupt_info(
    const wh::core::run_context &context)
    -> wh::core::result<interrupt_info> {
  if (!context.interrupt_info.has_value()) {
    return wh::core::result<interrupt_info>::failure(
        wh::core::errc::not_found);
  }
  return make_interrupt_info(
      *context.interrupt_info,
      context.resume_info.has_value() ? std::addressof(*context.resume_info)
                                      : nullptr);
}

/// Applies one ADK patch onto one interrupt context by lowering to compose resume
/// decisions.
[[nodiscard]] inline auto apply_interrupt_patch(
    wh::core::resume_state &state, const wh::core::interrupt_context &context,
    const interrupt_patch &patch) -> wh::core::result<void> {
  return wh::compose::apply_resume_decision(
      state, context,
      wh::compose::interrupt_resume_decision{
          .interrupt_context_id = context.interrupt_id,
          .decision = detail::to_compose_decision(patch.resolution),
          .edited_payload = patch.payload,
          .audit = detail::to_compose_audit(patch.audit),
      });
}

/// Applies one ordered ADK patch batch.
[[nodiscard]] inline auto apply_interrupt_patch_batch(
    wh::core::resume_state &state,
    const std::span<const wh::core::interrupt_context> contexts,
    const std::span<const interrupt_patch_item> items)
    -> wh::core::result<void> {
  for (const auto &item : items) {
    bool matched = false;
    for (const auto &context : contexts) {
      if (context.interrupt_id != item.interrupt_id) {
        continue;
      }
      auto applied = apply_interrupt_patch(state, context, item.patch);
      if (applied.has_error()) {
        return applied;
      }
      matched = true;
      break;
    }
    if (!matched) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
  }
  return {};
}

} // namespace wh::adk

namespace wh::core {

template <> struct any_owned_traits<wh::adk::interrupt_info> {
  [[nodiscard]] static auto into_owned(const wh::adk::interrupt_info &value)
      -> wh::core::result<wh::adk::interrupt_info> {
    auto state = wh::core::into_owned(value.state);
    if (state.has_error()) {
      return wh::core::result<wh::adk::interrupt_info>::failure(state.error());
    }
    auto payload = wh::core::into_owned(value.payload);
    if (payload.has_error()) {
      return wh::core::result<wh::adk::interrupt_info>::failure(payload.error());
    }
    return wh::adk::interrupt_info{
        .interrupt_id = value.interrupt_id,
        .run_path = value.run_path,
        .state = std::move(state).value(),
        .payload = std::move(payload).value(),
        .target_kind = value.target_kind,
        .used = value.used,
        .trigger_reason = value.trigger_reason,
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::interrupt_info &&value)
      -> wh::core::result<wh::adk::interrupt_info> {
    auto state = wh::core::into_owned(std::move(value.state));
    if (state.has_error()) {
      return wh::core::result<wh::adk::interrupt_info>::failure(state.error());
    }
    auto payload = wh::core::into_owned(std::move(value.payload));
    if (payload.has_error()) {
      return wh::core::result<wh::adk::interrupt_info>::failure(payload.error());
    }
    return wh::adk::interrupt_info{
        .interrupt_id = std::move(value.interrupt_id),
        .run_path = std::move(value.run_path),
        .state = std::move(state).value(),
        .payload = std::move(payload).value(),
        .target_kind = value.target_kind,
        .used = value.used,
        .trigger_reason = std::move(value.trigger_reason),
    };
  }
};

template <> struct any_owned_traits<wh::adk::interrupt_patch> {
  [[nodiscard]] static auto into_owned(const wh::adk::interrupt_patch &value)
      -> wh::core::result<wh::adk::interrupt_patch> {
    auto payload = wh::core::into_owned(value.payload);
    if (payload.has_error()) {
      return wh::core::result<wh::adk::interrupt_patch>::failure(payload.error());
    }
    return wh::adk::interrupt_patch{
        .resolution = value.resolution,
        .payload = std::move(payload).value(),
        .audit = value.audit,
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::interrupt_patch &&value)
      -> wh::core::result<wh::adk::interrupt_patch> {
    auto payload = wh::core::into_owned(std::move(value.payload));
    if (payload.has_error()) {
      return wh::core::result<wh::adk::interrupt_patch>::failure(payload.error());
    }
    return wh::adk::interrupt_patch{
        .resolution = value.resolution,
        .payload = std::move(payload).value(),
        .audit = std::move(value.audit),
    };
  }
};

} // namespace wh::core
