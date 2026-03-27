#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "wh/core/cursor_reader/detail/pull_op.hpp"
#include "wh/core/cursor_reader/detail/retained_ring_storage.hpp"
#include "wh/core/cursor_reader/detail/source.hpp"
#include "wh/core/cursor_reader/detail/waiter.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"

namespace wh::core::cursor_reader_detail {

template <wh::core::cursor_reader_source source_t, typename policy_t>
  requires wh::core::cursor_reader_detail::policy_for<source_t, policy_t>
class shared_state : public std::enable_shared_from_this<shared_state<source_t, policy_t>> {
public:
  using result_type = typename policy_t::result_type;
  using try_result_type = typename policy_t::try_result_type;
  using value_type = typename result_type::value_type;
  using storage_type = retained_ring_storage<result_type>;
  using sync_waiter_t = sync_waiter<result_type>;
  using async_waiter_t = async_waiter_base<result_type>;
  using reader_state_t = reader_state<result_type>;
  using sync_ready_buffer_t = sync_ready_buffer<result_type>;
  using async_ready_list_t = async_ready_list<result_type>;

  struct inactive_pull_op {};
  using pull_op_t =
      std::conditional_t<wh::core::cursor_reader_detail::async_source<source_t>,
                         pull_op<shared_state, source_t, result_type>,
                         inactive_pull_op>;

  struct async_read_ticket {
    std::optional<result_type> ready{};
    bool start_pull{false};
  };

public:
  explicit shared_state(source_t source, const std::size_t reader_count)
      : source_(std::move(source)),
        slots_(initial_capacity()),
        reader_counts_(initial_capacity() + 1U, 0U),
        capacity_(initial_capacity()),
        readers_(reader_count),
        open_reader_count_(reader_count) {
    policy_t::set_automatic_close(source_, automatic_close_);
    for (auto &reader : readers_) {
      reader.next_sequence = base_sequence_;
      increment_sequence_count_locked(reader.next_sequence);
    }
  }

  [[nodiscard]] auto async_failure(const wh::core::error_code code) const noexcept
      -> result_type {
    if constexpr (std::same_as<typename result_type::error_type,
                               wh::core::error_code>) {
      return result_type::failure(code);
    } else {
      return policy_t::internal_result();
    }
  }

  auto set_automatic_close(const bool enabled) -> void {
    std::scoped_lock lock(lock_);
    automatic_close_ = enabled;
    policy_t::set_automatic_close(source_, enabled);
  }

  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    std::scoped_lock lock(lock_);
    return source_closed_ || pull_state_ == pull_state::terminal;
  }

  [[nodiscard]] auto reader_is_closed(const std::size_t reader_index) const noexcept
      -> bool {
    std::scoped_lock lock(lock_);
    const auto &reader = readers_[reader_index];
    return reader.closed ||
           (pull_state_ == pull_state::terminal &&
            reader.next_sequence >= write_sequence_);
  }

  auto close_reader(const std::size_t reader_index) noexcept -> void {
    async_ready_list_t async_ready{};
    bool start_close = false;
    std::shared_ptr<shared_state> keepalive{};
    {
      std::scoped_lock lock(lock_);
      auto &reader = readers_[reader_index];
      if (reader.closed) {
        return;
      }
      reader.closed = true;
      decrement_sequence_count_locked(reader.next_sequence);
      while (auto *waiter = reader.sync_waiters.try_pop_front()) {
        waiter->complete(policy_t::closed_result());
      }
      while (auto *waiter = reader.async_waiters.try_pop_front()) {
        decrement_async_waiter_count_locked();
        waiter->store_ready(policy_t::closed_result());
        async_ready.push_back(waiter);
      }
      if (open_reader_count_ > 0U) {
        --open_reader_count_;
      }
      reclaim_prefix_locked();
      if (automatic_close_ && open_reader_count_ == 0U &&
          pull_state_ != pull_state::terminal) {
        close_requested_ = true;
        switch (pull_state_) {
        case pull_state::idle:
          pull_state_ = pull_state::closing;
          keepalive = this->shared_from_this();
          start_close = true;
          break;
        case pull_state::try_reading:
        case pull_state::blocking_reading:
        case pull_state::async_reading:
        case pull_state::closing:
          break;
        case pull_state::terminal:
          break;
        }
      }
    }

    if (start_close) {
      run_close_source();
    }
    notify_async_waiters(async_ready);
  }

  [[nodiscard]] auto try_read_for(const std::size_t reader_index)
      -> try_result_type {
    auto &reader = readers_[reader_index];
    for (;;) {
      {
        std::scoped_lock lock(lock_);
        if (reader.closed) {
          return policy_t::ready(policy_t::closed_result());
        }
        if (auto ready = consume_local_locked(reader); ready.has_value()) {
          return policy_t::ready(std::move(*ready));
        }
        if (pull_state_ == pull_state::terminal) {
          return policy_t::ready(policy_t::closed_result());
        }
        if (pull_state_ != pull_state::idle) {
          return policy_t::pending();
        }
        pull_state_ = pull_state::try_reading;
      }

      if (try_pull_once()) {
        continue;
      }

      {
        std::scoped_lock lock(lock_);
        if (reader.closed) {
          return policy_t::ready(policy_t::closed_result());
        }
        if (auto ready = consume_local_locked(reader); ready.has_value()) {
          return policy_t::ready(std::move(*ready));
        }
        if (pull_state_ == pull_state::terminal) {
          return policy_t::ready(policy_t::closed_result());
        }
      }
      return policy_t::pending();
    }
  }

  [[nodiscard]] auto read_for(const std::size_t reader_index) -> result_type {
    auto &reader = readers_[reader_index];
    for (;;) {
      sync_waiter_t waiter{};
      bool became_leader = false;
      {
        std::scoped_lock lock(lock_);
        if (reader.closed) {
          return policy_t::closed_result();
        }
        if (auto ready = consume_local_locked(reader); ready.has_value()) {
          return std::move(*ready);
        }
        if (pull_state_ == pull_state::terminal) {
          return policy_t::closed_result();
        }
        if (pull_state_ == pull_state::idle) {
          pull_state_ = pull_state::blocking_reading;
          became_leader = true;
        } else {
          reader.sync_waiters.push_back(&waiter);
        }
      }

      if (!became_leader) {
        return waiter.wait();
      }

      pull_once();
    }
  }

  [[nodiscard]] auto prepare_async_read(const std::size_t reader_index,
                                        async_waiter_t *waiter) noexcept
      -> async_read_ticket {
    std::scoped_lock lock(lock_);
    auto &reader = readers_[reader_index];
    if (reader.closed) {
      return {.ready = policy_t::closed_result(),
              .start_pull = false};
    }
    if (auto ready = consume_local_locked(reader); ready.has_value()) {
      return {.ready = std::move(*ready), .start_pull = false};
    }
    if (pull_state_ == pull_state::terminal) {
      return {.ready = policy_t::closed_result(),
              .start_pull = false};
    }

    reader.async_waiters.push_back(waiter);
    increment_async_waiter_count_locked();
    if (pull_state_ == pull_state::idle) {
      pull_state_ = pull_state::async_reading;
      return {.ready = std::nullopt, .start_pull = true};
    }
    return {.ready = std::nullopt, .start_pull = false};
  }

  auto remove_async_waiter(const std::size_t reader_index,
                           async_waiter_t *waiter) noexcept -> bool {
    std::scoped_lock lock(lock_);
    auto removed = readers_[reader_index].async_waiters.try_remove(waiter);
    if (!removed) {
      return false;
    }
    decrement_async_waiter_count_locked();
    if (pull_state_ == pull_state::async_reading && !active_pull_.has_value() &&
        async_waiter_count_ == 0U && !close_requested_) {
      pull_state_ = pull_state::idle;
    }
    return true;
  }

  auto start_async_pull(wh::core::detail::any_resume_scheduler_t scheduler) noexcept
      -> void
    requires wh::core::cursor_reader_detail::async_source<source_t>
  {
    try {
      auto keepalive = this->shared_from_this();
      {
        std::scoped_lock lock(lock_);
        if (active_pull_.has_value() || pull_state_ != pull_state::async_reading) {
          return;
        }
        pull_keepalive_ = std::move(keepalive);
        active_pull_.emplace_from(
            [](shared_state *owner) { return pull_op_t{owner}; }, this);
      }
      active_pull_.get().start(source_, std::move(scheduler));
    } catch (...) {
      finish_source_pull(nullptr, policy_t::internal_result(), true);
      reset_active_pull(nullptr);
    }
  }

  auto finish_source_pull(pull_op_t *pull, result_type status,
                          const bool terminal_override) noexcept -> void {
    bool discard_result = false;
    bool start_close = false;
    std::shared_ptr<shared_state> keepalive{};
    {
      std::scoped_lock lock(lock_);
      if (pull != nullptr) {
        if (!active_pull_.has_value() || std::addressof(active_pull_.get()) != pull) {
          return;
        }
      } else if (!active_pull_.has_value()) {
        return;
      }
      if (open_reader_count_ == 0U && close_requested_) {
        pull_state_ = pull_state::closing;
        discard_result = true;
        keepalive = this->shared_from_this();
        start_close = true;
      }
    }
    if (start_close) {
      run_close_source();
    }
    if (discard_result) {
      return;
    }
    publish(std::move(status), terminal_override || policy_t::is_terminal(status));
  }

  auto reset_active_pull(const pull_op_t *pull) noexcept -> void {
    std::scoped_lock lock(lock_);
    if (!active_pull_.has_value()) {
      return;
    }
    if (pull != nullptr && std::addressof(active_pull_.get()) != pull) {
      return;
    }
    active_pull_.reset();
    pull_keepalive_.reset();
  }

private:
  [[nodiscard]] static constexpr auto initial_capacity() noexcept -> std::size_t {
    return 4U;
  }

  auto increment_async_waiter_count_locked() noexcept -> void {
    ++async_waiter_count_;
  }

  auto decrement_async_waiter_count_locked() noexcept -> void {
    if (async_waiter_count_ > 0U) {
      --async_waiter_count_;
    }
  }

  auto run_close_source() noexcept -> void {
    try {
      [[maybe_unused]] const auto close_status = source_.close();
    } catch (...) {
    }
    finish_source_close();
  }

  auto finish_source_close() noexcept -> void {
    std::scoped_lock lock(lock_);
    source_closed_ = true;
    close_requested_ = false;
    pull_state_ = pull_state::terminal;
  }

  [[nodiscard]] auto try_pull_once() -> bool {
    result_type published{};
    bool terminal = false;
    bool start_close = false;
    std::shared_ptr<shared_state> keepalive{};
    try {
      auto next = source_.try_read();
      if (policy_t::is_pending(next)) {
        {
          std::scoped_lock lock(lock_);
          if (close_requested_) {
            pull_state_ = pull_state::closing;
            keepalive = this->shared_from_this();
            start_close = true;
          } else {
            pull_state_ = pull_state::idle;
          }
        }
        if (start_close) {
          run_close_source();
        }
        return false;
      }
      published = policy_t::project_try(std::move(next));
      terminal = policy_t::is_terminal(published);
    } catch (...) {
      published = policy_t::internal_result();
      terminal = true;
    }

    publish(std::move(published), terminal);
    return true;
  }

  auto pull_once() -> void {
    result_type published{};
    bool terminal = false;
    try {
      auto next = source_.read();
      if (next.has_error()) {
        published = result_type::failure(next.error());
        terminal = true;
      } else {
        published = std::move(next);
        terminal = policy_t::is_terminal(published);
      }
    } catch (...) {
      published = policy_t::internal_result();
      terminal = true;
    }

    publish(std::move(published), terminal);
  }

  auto publish(result_type status, const bool terminal) noexcept -> void {
    sync_ready_buffer_t sync_ready{};
    async_ready_list_t async_ready{};
    bool start_close = false;
    std::shared_ptr<shared_state> keepalive{};
    {
      std::scoped_lock lock(lock_);
      ensure_capacity_locked();
      slots_.construct_at_sequence(write_sequence_, status);
      ++write_sequence_;
      if (terminal) {
        source_closed_ = true;
        close_requested_ = false;
        pull_state_ = pull_state::terminal;
      } else if (close_requested_) {
        pull_state_ = pull_state::closing;
        keepalive = this->shared_from_this();
        start_close = true;
      } else {
        pull_state_ = pull_state::idle;
      }
      wake_ready_waiters_locked(sync_ready, async_ready);
    }
    notify_sync_waiters(sync_ready);
    notify_async_waiters(async_ready);
    if (start_close) {
      run_close_source();
    }
  }

  auto wake_ready_waiters_locked(sync_ready_buffer_t &sync_ready,
                                 async_ready_list_t &async_ready) -> void {
    for (auto &reader : readers_) {
      while (reader.sync_waiters.front() != nullptr) {
        if (auto ready = consume_local_locked(reader); ready.has_value()) {
          auto *waiter = reader.sync_waiters.try_pop_front();
          waiter->status.emplace(std::move(*ready));
          sync_ready.push_back(waiter);
          continue;
        }
        if (pull_state_ == pull_state::terminal) {
          auto *waiter = reader.sync_waiters.try_pop_front();
          waiter->status.emplace(policy_t::closed_result());
          sync_ready.push_back(waiter);
        }
        break;
      }

      while (reader.async_waiters.front() != nullptr) {
        if (auto ready = consume_local_locked(reader); ready.has_value()) {
          auto *waiter = reader.async_waiters.try_pop_front();
          decrement_async_waiter_count_locked();
          waiter->store_ready(std::move(*ready));
          async_ready.push_back(waiter);
          continue;
        }
        if (pull_state_ == pull_state::terminal) {
          auto *waiter = reader.async_waiters.try_pop_front();
          decrement_async_waiter_count_locked();
          waiter->store_ready(policy_t::closed_result());
          async_ready.push_back(waiter);
        }
        break;
      }
    }
  }

  [[nodiscard]] auto consume_local_locked(reader_state_t &reader)
      -> std::optional<result_type> {
    if (reader.closed || reader.next_sequence >= write_sequence_) {
      return std::nullopt;
    }

    auto result = slots_.value_at_sequence(reader.next_sequence);
    const auto consumed_sequence = reader.next_sequence;
    decrement_sequence_count_locked(consumed_sequence);
    ++reader.next_sequence;
    increment_sequence_count_locked(reader.next_sequence);
    reclaim_prefix_locked();
    return result;
  }

  auto ensure_capacity_locked() -> void {
    if ((write_sequence_ - base_sequence_) < capacity_) {
      return;
    }
    grow_locked(std::max<std::size_t>(capacity_ * 2U, capacity_ + 1U));
  }

  auto grow_locked(const std::size_t new_capacity) -> void {
    sequence_count_buffer next_counts{};
    next_counts.resize(new_capacity + 1U, 0U);
    for (auto sequence = base_sequence_; sequence <= write_sequence_; ++sequence) {
      next_counts[static_cast<std::size_t>(sequence - base_sequence_)] =
          sequence_count_at_locked(sequence);
    }
    slots_.reserve(new_capacity, base_sequence_, write_sequence_);
    reader_counts_ = std::move(next_counts);
    reader_counts_base_ = 0U;
    capacity_ = new_capacity;
  }

  auto reclaim_prefix_locked() noexcept -> void {
    while (base_sequence_ < write_sequence_ &&
           sequence_count_at_locked(base_sequence_) == 0U) {
      slots_.destroy_at_sequence(base_sequence_);
      clear_sequence_count_locked(base_sequence_);
      ++base_sequence_;
      advance_counts_base_locked();
    }
  }

  [[nodiscard]] auto sequence_count_index_locked(
      const std::uint64_t sequence) const noexcept -> std::size_t {
    const auto span = capacity_ + 1U;
    return static_cast<std::size_t>(
        (reader_counts_base_ + (sequence - base_sequence_)) % span);
  }

  [[nodiscard]] auto sequence_count_at_locked(
      const std::uint64_t sequence) const noexcept -> std::size_t {
    return reader_counts_[sequence_count_index_locked(sequence)];
  }

  auto increment_sequence_count_locked(const std::uint64_t sequence) noexcept
      -> void {
    ++reader_counts_[sequence_count_index_locked(sequence)];
  }

  auto decrement_sequence_count_locked(const std::uint64_t sequence) noexcept
      -> void {
    auto &count = reader_counts_[sequence_count_index_locked(sequence)];
    if (count > 0U) {
      --count;
    }
  }

  auto clear_sequence_count_locked(const std::uint64_t sequence) noexcept -> void {
    reader_counts_[sequence_count_index_locked(sequence)] = 0U;
  }

  auto advance_counts_base_locked() noexcept -> void {
    const auto span = capacity_ + 1U;
    reader_counts_base_ = (reader_counts_base_ + 1U) % span;
  }

  static auto notify_sync_waiters(const sync_ready_buffer_t &ready_waiters) -> void {
    for (auto *waiter : ready_waiters) {
      waiter->ready.test_and_set(std::memory_order_release);
      waiter->ready.notify_one();
    }
  }

  static auto notify_async_waiters(async_ready_list_t &ready_waiters) -> void {
    ready_waiters.complete_all();
  }

  mutable std::mutex lock_{};
  source_t source_{};
  storage_type slots_;
  sequence_count_buffer reader_counts_{};
  std::size_t capacity_{0U};
  std::uint64_t base_sequence_{0U};
  std::uint64_t write_sequence_{0U};
  std::size_t reader_counts_base_{0U};
  pull_state pull_state_{pull_state::idle};
  bool close_requested_{false};
  bool source_closed_{false};
  bool automatic_close_{true};
  std::vector<reader_state_t> readers_{};
  std::size_t open_reader_count_{0U};
  std::size_t async_waiter_count_{0U};
  wh::core::detail::manual_lifetime_box<pull_op_t> active_pull_{};
  std::shared_ptr<shared_state> pull_keepalive_{};
};

} // namespace wh::core::cursor_reader_detail
