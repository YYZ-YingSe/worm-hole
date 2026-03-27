#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

#include "wh/core/cursor_reader/detail/pull_state.hpp"
#include "wh/core/cursor_reader/detail/waiter_ready_list.hpp"
#include "wh/core/cursor_reader/detail/waiter_list.hpp"
#include "wh/core/small_vector.hpp"

namespace wh::core::cursor_reader_detail {

template <typename waiter_t> struct waiter_ops {
  void (*complete)(waiter_t *) noexcept {nullptr};
};

template <typename result_t> struct sync_waiter {
  sync_waiter *next{nullptr};
  sync_waiter *prev{nullptr};
  std::atomic_flag ready = ATOMIC_FLAG_INIT;
  std::optional<result_t> status{};

  auto complete(result_t value) noexcept -> void {
    status.emplace(std::move(value));
    ready.test_and_set(std::memory_order_release);
    ready.notify_one();
  }

  [[nodiscard]] auto wait() -> result_t {
    ready.wait(false, std::memory_order_acquire);
    return std::move(*status);
  }
};

template <typename result_t> struct async_waiter_base {
  using ops_type = waiter_ops<async_waiter_base>;

  async_waiter_base *next{nullptr};
  async_waiter_base *prev{nullptr};
  const ops_type *ops{nullptr};
  std::optional<result_t> status{};

  auto store_ready(result_t value) noexcept -> void {
    status.emplace(std::move(value));
  }

  [[nodiscard]] auto take_ready() -> result_t {
    return std::move(*status);
  }
};

template <typename result_t> struct reader_state {
  std::uint64_t next_sequence{0U};
  bool closed{false};
  waiter_list<sync_waiter<result_t>> sync_waiters{};
  waiter_list<async_waiter_base<result_t>> async_waiters{};
};

using sequence_count_buffer = wh::core::small_vector<std::size_t, 9U>;

template <typename result_t>
using sync_ready_buffer = wh::core::small_vector<sync_waiter<result_t> *, 8U>;

template <typename result_t>
using async_ready_list = waiter_ready_list<async_waiter_base<result_t>>;

} // namespace wh::core::cursor_reader_detail
