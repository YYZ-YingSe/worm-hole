// Defines process-state runtime helpers extracted from graph execution core.
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "wh/compose/graph/keys.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::process_runtime {

using node_local_process_state_slots = std::vector<std::optional<graph_process_state>>;

/// RAII holder for one node-local process-state lifecycle.
class scoped_node_local_process_state {
public:
  scoped_node_local_process_state() = default;

  explicit scoped_node_local_process_state(const std::uint32_t node_id) noexcept
      : node_id_(node_id) {}

  scoped_node_local_process_state(const scoped_node_local_process_state &) = delete;
  auto operator=(const scoped_node_local_process_state &)
      -> scoped_node_local_process_state & = delete;

  scoped_node_local_process_state(scoped_node_local_process_state &&) noexcept = default;

  auto operator=(scoped_node_local_process_state &&) noexcept
      -> scoped_node_local_process_state & = default;

  ~scoped_node_local_process_state() = default;

  [[nodiscard]] auto get(node_local_process_state_slots &states)
      -> wh::core::result<std::reference_wrapper<graph_process_state>> {
    if (node_id_ == invalid_node_id || node_id_ >= states.size()) {
      return wh::core::result<std::reference_wrapper<graph_process_state>>::failure(
          wh::core::errc::contract_violation);
    }
    auto &slot = states[node_id_];
    if (!slot.has_value()) {
      return wh::core::result<std::reference_wrapper<graph_process_state>>::failure(
          wh::core::errc::not_found);
    }
    return std::ref(*slot);
  }

  auto release(node_local_process_state_slots &states) noexcept -> void {
    if (node_id_ != invalid_node_id && node_id_ < states.size()) {
      states[node_id_].reset();
    }
    node_id_ = invalid_node_id;
  }

private:
  static constexpr std::uint32_t invalid_node_id = std::numeric_limits<std::uint32_t>::max();

  std::uint32_t node_id_{invalid_node_id};
};

[[nodiscard]] inline auto
acquire_node_local_process_state(node_local_process_state_slots &states,
                                 const std::uint32_t node_id,
                                 graph_process_state &parent_process_state)
    -> wh::core::result<scoped_node_local_process_state> {
  if (node_id >= states.size()) {
    return wh::core::result<scoped_node_local_process_state>::failure(
        wh::core::errc::contract_violation);
  }
  auto &slot = states[node_id];
  slot.emplace(&parent_process_state);
  slot->set_parent(&parent_process_state);
  slot->set_workflow_scope_root(std::addressof(parent_process_state.workflow_scope_root()));
  return scoped_node_local_process_state{node_id};
}

inline auto bind_parent_process_state(graph_process_state &process_state,
                                      graph_process_state *parent) noexcept -> void {
  process_state.set_parent(parent);
  process_state.set_workflow_scope_root(std::addressof(process_state));
}

} // namespace wh::compose::detail::process_runtime
