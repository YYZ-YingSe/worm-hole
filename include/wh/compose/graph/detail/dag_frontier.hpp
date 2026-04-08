// Defines DAG frontier storage and wave-promotion helpers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "wh/compose/graph/detail/bitset.hpp"

namespace wh::compose::detail {

class dag_frontier {
public:
  auto reset(const std::size_t node_count) -> void {
    current_nodes_.clear();
    next_nodes_.clear();
    current_head_ = 0U;
    queued_.reset(node_count, false);
  }

  [[nodiscard]] auto enqueue_current(const std::uint32_t node_id) -> bool {
    wh_precondition(node_id < queued_.size());
    if (!queued_.set_if_unset(node_id)) {
      return false;
    }
    current_nodes_.push_back(node_id);
    return true;
  }

  [[nodiscard]] auto enqueue_next(const std::uint32_t node_id) -> bool {
    wh_precondition(node_id < queued_.size());
    if (!queued_.set_if_unset(node_id)) {
      return false;
    }
    next_nodes_.push_back(node_id);
    return true;
  }

  [[nodiscard]] auto dequeue() -> std::optional<std::uint32_t> {
    wh_invariant(current_head_ <= current_nodes_.size());
    if (current_head_ >= current_nodes_.size()) {
      return std::nullopt;
    }
    const auto node_id = current_nodes_[current_head_++];
    queued_.clear(node_id);
    return node_id;
  }

  [[nodiscard]] auto promote_next_wave() -> bool {
    wh_invariant(current_head_ <= current_nodes_.size());
    if (next_nodes_.empty()) {
      return false;
    }
    current_nodes_.clear();
    current_head_ = 0U;
    current_nodes_.swap(next_nodes_);
    return true;
  }

private:
  std::vector<std::uint32_t> current_nodes_{};
  std::vector<std::uint32_t> next_nodes_{};
  std::size_t current_head_{0U};
  wh::compose::detail::dynamic_bitset queued_{};
};

} // namespace wh::compose::detail
