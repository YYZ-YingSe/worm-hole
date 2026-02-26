#pragma once

#include <atomic>
#include <thread>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/scheduler/context_helper.hpp"

namespace wh::testing {

class static_thread_scheduler_helper {
public:
  using scheduler_type =
      decltype(std::declval<stdexec::run_loop &>().get_scheduler());
  using context_type = wh::core::scheduler_context<scheduler_type>;

  static_thread_scheduler_helper() : worker_([this] { run_loop_.run(); }) {}

  static_thread_scheduler_helper(const static_thread_scheduler_helper &) =
      delete;
  auto operator=(const static_thread_scheduler_helper &)
      -> static_thread_scheduler_helper & = delete;

  static_thread_scheduler_helper(static_thread_scheduler_helper &&) = delete;
  auto operator=(static_thread_scheduler_helper &&)
      -> static_thread_scheduler_helper & = delete;

  ~static_thread_scheduler_helper() { stop(); }

  [[nodiscard]] auto context() -> context_type {
    return wh::core::make_scheduler_context(run_loop_.get_scheduler());
  }

  auto stop() -> void {
    const auto was_running =
        running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running) {
      return;
    }
    run_loop_.finish();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  stdexec::run_loop run_loop_{};
  std::thread worker_{};
  std::atomic<bool> running_{true};
};

} // namespace wh::testing
