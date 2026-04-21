#pragma once

#include <thread>
#include <utility>

#include <stdexec/execution.hpp>

namespace wh::testing::helper {

// Prefer stdexec stop primitives in tests so older Apple libc++ SDK
// availability does not gate CI on std::stop_token/std::stop_source.
using stop_source = stdexec::inplace_stop_source;
using stop_token = stdexec::inplace_stop_token;

class joining_thread {
public:
  joining_thread() noexcept = default;

  template <typename function_t, typename... args_t>
  explicit joining_thread(function_t &&function, args_t &&...args)
      : thread_(std::forward<function_t>(function),
                std::forward<args_t>(args)...) {}

  joining_thread(const joining_thread &) = delete;
  auto operator=(const joining_thread &) -> joining_thread & = delete;

  joining_thread(joining_thread &&) noexcept = default;

  auto operator=(joining_thread &&other) noexcept -> joining_thread & {
    if (this != &other) {
      join_if_needed();
      thread_ = std::move(other.thread_);
    }
    return *this;
  }

  ~joining_thread() { join_if_needed(); }

  [[nodiscard]] auto joinable() const noexcept -> bool {
    return thread_.joinable();
  }

  auto join() -> void { thread_.join(); }

private:
  auto join_if_needed() noexcept -> void {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::thread thread_{};
};

} // namespace wh::testing::helper
