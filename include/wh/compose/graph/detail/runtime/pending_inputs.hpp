// Defines dense pending-input runtime storage keyed by compile-time node id.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "wh/compose/graph/detail/bitset.hpp"
#include "wh/compose/types.hpp"

namespace wh::compose::detail::runtime_state {

class pending_inputs {
public:
  pending_inputs() = default;

  auto reset(const std::size_t node_count) -> void {
    inputs_.clear();
    inputs_.resize(node_count);
    present_.reset(node_count);
    restored_.reset(node_count);
    restored_input_.reset(node_count);
    active_count_ = 0U;
  }

  [[nodiscard]] auto contains_input(const std::uint32_t node_id) const noexcept -> bool {
    return node_id < inputs_.size() && present_.test(node_id);
  }

  [[nodiscard]] auto active_input_count() const noexcept -> std::size_t { return active_count_; }

  [[nodiscard]] auto find_input(const std::uint32_t node_id) noexcept -> graph_value * {
    if (!contains_input(node_id)) {
      return nullptr;
    }
    return std::addressof(inputs_[node_id]);
  }

  [[nodiscard]] auto find_input(const std::uint32_t node_id) const noexcept -> const graph_value * {
    if (!contains_input(node_id)) {
      return nullptr;
    }
    return std::addressof(inputs_[node_id]);
  }

  auto store_input(const std::uint32_t node_id, graph_value value) -> void {
    if (!present_.test(node_id)) {
      ++active_count_;
    }
    inputs_[node_id] = std::move(value);
    present_.set(node_id);
  }

  auto mark_restored_node(const std::uint32_t node_id) noexcept -> void { restored_.set(node_id); }

  [[nodiscard]] auto restored_node(const std::uint32_t node_id) const noexcept -> bool {
    return node_id < inputs_.size() && restored_.test(node_id);
  }

  auto mark_restored_input(const std::uint32_t node_id) noexcept -> void {
    restored_input_.set(node_id);
  }

  [[nodiscard]] auto restored_input(const std::uint32_t node_id) const noexcept -> bool {
    return node_id < inputs_.size() && restored_input_.test(node_id);
  }

private:
  std::vector<graph_value> inputs_{};
  dynamic_bitset present_{};
  dynamic_bitset restored_{};
  dynamic_bitset restored_input_{};
  std::size_t active_count_{0U};
};

} // namespace wh::compose::detail::runtime_state
