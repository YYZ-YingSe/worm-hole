// Defines Pregel-specific runtime delivery and frontier types.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "wh/compose/graph/detail/bitset.hpp"

namespace wh::compose::detail::input_runtime {

enum class pregel_ready_state : std::uint8_t { ready, skipped };

struct pregel_node_inputs {
  std::vector<std::uint32_t> control_edges{};
  std::vector<std::uint32_t> data_edges{};

  auto reset() -> void {
    control_edges.clear();
    data_edges.clear();
  }
};

struct pregel_delivery_store {
  std::vector<pregel_node_inputs> current{};
  std::vector<pregel_node_inputs> next{};
  std::vector<std::uint32_t> current_nodes{};
  std::vector<std::uint32_t> next_nodes{};
  wh::compose::detail::dynamic_bitset current_enqueued{};
  wh::compose::detail::dynamic_bitset next_enqueued{};

  auto reset(const std::size_t node_count) -> void {
    if (current.size() < node_count) {
      current.resize(node_count);
    }
    if (next.size() < node_count) {
      next.resize(node_count);
    }
    clear_storage(current, current_nodes);
    clear_storage(next, next_nodes);
    current_enqueued.reset(node_count, false);
    next_enqueued.reset(node_count, false);
  }

  auto stage_current_control(const std::uint32_t node_id,
                             const std::uint32_t edge_id) -> void {
    mark_current(node_id);
    current[node_id].control_edges.push_back(edge_id);
  }

  auto stage_current_node(const std::uint32_t node_id) -> void {
    mark_current(node_id);
  }

  auto stage_current_data(const std::uint32_t node_id,
                          const std::uint32_t edge_id) -> void {
    mark_current(node_id);
    current[node_id].data_edges.push_back(edge_id);
  }

  auto stage_next_control(const std::uint32_t node_id,
                          const std::uint32_t edge_id) -> void {
    mark_next(node_id);
    next[node_id].control_edges.push_back(edge_id);
  }

  auto stage_next_node(const std::uint32_t node_id) -> void { mark_next(node_id); }

  auto stage_next_data(const std::uint32_t node_id,
                       const std::uint32_t edge_id) -> void {
    mark_next(node_id);
    next[node_id].data_edges.push_back(edge_id);
  }

  [[nodiscard]] auto current_frontier() const -> const std::vector<std::uint32_t> & {
    return current_nodes;
  }

  auto clear_current_node(const std::uint32_t node_id) -> void {
    current[node_id].reset();
    current_enqueued.clear(node_id);
  }

  auto advance_superstep() -> std::vector<std::uint32_t> {
    clear_storage(current, current_nodes);
    current.swap(next);
    current_nodes.swap(next_nodes);
    current_enqueued.swap(next_enqueued);
    next_enqueued.reset(current_enqueued.size(), false);
    return current_nodes;
  }

private:
  auto mark_current(const std::uint32_t node_id) -> void {
    if (current_enqueued.set_if_unset(node_id)) {
      current_nodes.push_back(node_id);
    }
  }

  auto mark_next(const std::uint32_t node_id) -> void {
    if (next_enqueued.set_if_unset(node_id)) {
      next_nodes.push_back(node_id);
    }
  }

  static auto clear_storage(std::vector<pregel_node_inputs> &storage,
                            std::vector<std::uint32_t> &active_nodes) -> void {
    for (const auto node_id : active_nodes) {
      storage[node_id].reset();
    }
    active_nodes.clear();
  }
};

} // namespace wh::compose::detail::input_runtime
