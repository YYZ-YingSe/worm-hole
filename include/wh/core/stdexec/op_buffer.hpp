// Defines reusable manual-lifetime op-state storage with retained capacity.
#pragma once

#include <cstddef>
#include <memory>

#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace wh::core::detail {

template <typename op_t> class op_buffer {
public:
  op_buffer() = default;
  op_buffer(const op_buffer &) = delete;
  auto operator=(const op_buffer &) -> op_buffer & = delete;

  ~op_buffer() { reset(); }

  auto ensure(const std::size_t count) -> void {
    if (count <= capacity_) {
      return;
    }
    slots_ = std::make_unique<manual_lifetime_box<op_t>[]>(count);
    capacity_ = count;
    active_ = 0U;
  }

  auto reset() noexcept -> void {
    for (std::size_t index = 0U; index < active_; ++index) {
      slots_[index].reset();
    }
    active_ = 0U;
  }

  [[nodiscard]] auto operator[](const std::size_t index) noexcept
      -> manual_lifetime_box<op_t> & {
    if (active_ <= index) {
      active_ = index + 1U;
    }
    return slots_[index];
  }

private:
  std::unique_ptr<manual_lifetime_box<op_t>[]> slots_{};
  std::size_t capacity_{0U};
  std::size_t active_{0U};
};

} // namespace wh::core::detail
