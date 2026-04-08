// Defines a shared-controller operation-state wrapper used by heap-owned drive
// loops.
#pragma once

#include <memory>
#include <stdexec/execution.hpp>

namespace wh::core::detail {

template <typename controller_t> class shared_operation_state {
public:
  using operation_state_concept = stdexec::operation_state_t;

  explicit shared_operation_state(
      std::shared_ptr<controller_t> controller) noexcept
      : controller_(std::move(controller)) {}

  auto start() & noexcept -> void { controller_->start(); }

private:
  std::shared_ptr<controller_t> controller_{};
};

} // namespace wh::core::detail
