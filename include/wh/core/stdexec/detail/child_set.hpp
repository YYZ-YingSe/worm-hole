// Defines shared child-op storage and completion collection for bounded sender
// fan-out loops.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/op_buffer.hpp"

namespace wh::core::detail {

template <typename child_op_t> class child_set {
public:
  child_set() = default;

  explicit child_set(const std::size_t size) { reset(size); }

  auto reset(const std::size_t size) -> void {
    child_ops_.reset();
    child_ops_.ensure(size);
    engaged_.assign(size, false);
    active_count_ = 0U;
  }

  auto destroy_all() noexcept -> void {
    child_ops_.reset();
    std::fill(engaged_.begin(), engaged_.end(), false);
    active_count_ = 0U;
  }

  template <typename start_fn_t>
  auto start_child(const std::uint32_t slot_id, start_fn_t &&start_fn)
      -> wh::core::result<void> {
    wh_precondition(slot_id < engaged_.size());
    wh_invariant(!engaged_[slot_id]);

    auto &slot = child_ops_[slot_id];
    try {
      std::invoke(std::forward<start_fn_t>(start_fn), slot);
      engaged_[slot_id] = true;
      ++active_count_;
      stdexec::start(slot.get());
      return {};
    } catch (...) {
      slot.reset();
      return wh::core::result<void>::failure(
          wh::core::map_current_exception());
    }
  }

  [[nodiscard]] auto active_count() const noexcept -> std::size_t {
    return active_count_;
  }

  auto reclaim_child(const std::uint32_t slot_id) noexcept -> void {
    wh_precondition(slot_id < engaged_.size());
    wh_invariant(engaged_[slot_id]);
    wh_invariant(active_count_ != 0U);
    child_ops_[slot_id].reset();
    engaged_[slot_id] = false;
    --active_count_;
  }

private:
  op_buffer<child_op_t> child_ops_{};
  std::vector<bool> engaged_{};
  std::size_t active_count_{0U};
};

} // namespace wh::core::detail
