#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/core/resume_state.hpp"

namespace wh::compose {

namespace detail {

[[nodiscard]] inline auto next_interrupt_sequence() noexcept -> std::uint64_t {
  static std::atomic<std::uint64_t> sequence{1U};
  return sequence.fetch_add(1U, std::memory_order_relaxed);
}

} // namespace detail

[[nodiscard]] inline auto make_interrupt_id() -> std::string {
  return std::string{"interrupt-"} +
         std::to_string(detail::next_interrupt_sequence());
}

[[nodiscard]] inline auto
make_interrupt_signal(std::string interrupt_id, wh::core::address location,
                      std::any state = {}, std::any layer_payload = {})
    -> wh::core::interrupt_signal {
  return wh::core::interrupt_signal{std::move(interrupt_id),
                                    std::move(location), std::move(state),
                                    std::move(layer_payload), false};
}

[[nodiscard]] inline auto make_interrupt_signal(wh::core::address location,
                                                std::any state = {},
                                                std::any layer_payload = {})
    -> wh::core::interrupt_signal {
  return make_interrupt_signal(make_interrupt_id(), std::move(location),
                               std::move(state), std::move(layer_payload));
}

template <typename state_t, typename payload_t>
[[nodiscard]] inline auto
make_interrupt_signal(std::string interrupt_id, wh::core::address location,
                      state_t &&state, payload_t &&layer_payload)
    -> wh::core::interrupt_signal {
  using state_value_t = std::remove_cvref_t<state_t>;
  using payload_value_t = std::remove_cvref_t<payload_t>;

  std::any boxed_state;
  if constexpr (std::same_as<state_value_t, std::any>) {
    boxed_state = std::forward<state_t>(state);
  } else {
    boxed_state = std::any{std::in_place_type<state_value_t>,
                           std::forward<state_t>(state)};
  }

  std::any boxed_payload;
  if constexpr (std::same_as<payload_value_t, std::any>) {
    boxed_payload = std::forward<payload_t>(layer_payload);
  } else {
    boxed_payload = std::any{std::in_place_type<payload_value_t>,
                             std::forward<payload_t>(layer_payload)};
  }

  return wh::core::interrupt_signal{std::move(interrupt_id),
                                    std::move(location), std::move(boxed_state),
                                    std::move(boxed_payload), false};
}

template <typename state_t, typename payload_t>
[[nodiscard]] inline auto make_interrupt_signal(wh::core::address location,
                                                state_t &&state,
                                                payload_t &&layer_payload)
    -> wh::core::interrupt_signal {
  return make_interrupt_signal(make_interrupt_id(), std::move(location),
                               std::forward<state_t>(state),
                               std::forward<payload_t>(layer_payload));
}

template <typename state_t>
[[nodiscard]] inline auto make_interrupt_signal(std::string interrupt_id,
                                                wh::core::address location,
                                                state_t &&state)
    -> wh::core::interrupt_signal {
  return make_interrupt_signal(std::move(interrupt_id), std::move(location),
                               std::forward<state_t>(state), std::any{});
}

template <typename state_t>
[[nodiscard]] inline auto make_interrupt_signal(wh::core::address location,
                                                state_t &&state)
    -> wh::core::interrupt_signal {
  return make_interrupt_signal(make_interrupt_id(), std::move(location),
                               std::forward<state_t>(state), std::any{});
}

[[nodiscard]] inline auto
to_interrupt_context(const wh::core::interrupt_signal &signal)
    -> wh::core::interrupt_context {
  return wh::core::to_interrupt_context(signal);
}

[[nodiscard]] inline auto flatten_interrupt_signals(
    const std::span<const wh::core::interrupt_signal> signals)
    -> wh::core::interrupt_snapshot {
  return wh::core::flatten_interrupt_signals(signals);
}

[[nodiscard]] inline auto
flatten_interrupt_signals(std::vector<wh::core::interrupt_signal> &&signals)
    -> wh::core::interrupt_snapshot {
  return wh::core::flatten_interrupt_signals(std::move(signals));
}

[[nodiscard]] inline auto rebuild_interrupt_signal_tree(
    const std::span<const wh::core::interrupt_signal> signals)
    -> std::vector<wh::core::interrupt_signal_tree_node> {
  return wh::core::rebuild_interrupt_signal_tree(signals);
}

[[nodiscard]] inline auto rebuild_interrupt_context_tree(
    const std::span<const wh::core::interrupt_context> contexts)
    -> std::vector<wh::core::interrupt_context_tree_node> {
  return wh::core::rebuild_interrupt_context_tree(contexts);
}

[[nodiscard]] inline auto to_interrupt_context_tree(
    const std::span<const wh::core::interrupt_signal_tree_node> roots)
    -> std::vector<wh::core::interrupt_context_tree_node> {
  return wh::core::to_interrupt_context_tree(roots);
}

[[nodiscard]] inline auto to_interrupt_signal_tree(
    const std::span<const wh::core::interrupt_context_tree_node> roots)
    -> std::vector<wh::core::interrupt_signal_tree_node> {
  return wh::core::to_interrupt_signal_tree(roots);
}

[[nodiscard]] inline auto flatten_interrupt_signal_tree(
    const std::span<const wh::core::interrupt_signal_tree_node> roots)
    -> std::vector<wh::core::interrupt_signal> {
  return wh::core::flatten_interrupt_signal_tree(roots);
}

[[nodiscard]] inline auto project_interrupt_context(
    const wh::core::interrupt_context &context,
    const std::span<const std::string_view> allowed_segments)
    -> wh::core::interrupt_context {
  return wh::core::project_interrupt_context(context, allowed_segments);
}

[[nodiscard]] inline auto project_interrupt_context(
    wh::core::interrupt_context &&context,
    const std::span<const std::string_view> allowed_segments)
    -> wh::core::interrupt_context {
  return wh::core::project_interrupt_context(std::move(context),
                                             allowed_segments);
}

} // namespace wh::compose
