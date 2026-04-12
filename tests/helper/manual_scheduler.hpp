#pragma once

#include <cstddef>
#include <concepts>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec.hpp"

namespace wh::testing::helper {

struct manual_task {
  manual_task *next{nullptr};
  virtual ~manual_task() = default;
  virtual auto execute() noexcept -> void = 0;
};

class manual_scheduler_state {
public:
  bool allow_try_schedule{true};
  bool inline_try_schedule{false};
  bool same_scheduler{false};
  std::size_t inline_try_schedule_calls{0U};

  auto enqueue(manual_task *task) noexcept -> void {
    task->next = nullptr;
    if (tail_ != nullptr) {
      tail_->next = task;
    } else {
      head_ = task;
    }
    tail_ = task;
    ++pending_;
  }

  [[nodiscard]] auto pending_count() const noexcept -> std::size_t {
    return pending_;
  }

  auto run_one() noexcept -> bool {
    if (head_ == nullptr) {
      return false;
    }
    auto *task = head_;
    head_ = head_->next;
    if (head_ == nullptr) {
      tail_ = nullptr;
    }
    task->next = nullptr;
    --pending_;
    task->execute();
    return true;
  }

  auto run_all() noexcept -> void {
    while (run_one()) {
    }
  }

private:
  manual_task *head_{nullptr};
  manual_task *tail_{nullptr};
  std::size_t pending_{0U};
};

template <typename would_block_t = void> class manual_scheduler;

template <> class manual_scheduler<void> {
public:
  manual_scheduler_state *state{nullptr};

  using scheduler_concept = stdexec::scheduler_t;

  template <typename receiver_t> struct schedule_op final : manual_task {
    using operation_state_concept = stdexec::operation_state_t;

    manual_scheduler_state *state{nullptr};
    receiver_t receiver;

    schedule_op(manual_scheduler_state *state_ptr, receiver_t receiver_value)
        : state(state_ptr), receiver(std::move(receiver_value)) {}

    auto start() noexcept -> void { state->enqueue(this); }

    auto execute() noexcept -> void override {
      stdexec::set_value(std::move(receiver));
    }
  };

  struct schedule_sender {
    manual_scheduler_state *state{nullptr};

    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;

    template <typename receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) const noexcept
        -> schedule_op<receiver_t> {
      return schedule_op<receiver_t>{state, std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  [[nodiscard]] auto schedule() const noexcept -> schedule_sender {
    return {state};
  }

  [[nodiscard]] auto query(
      wh::core::detail::scheduler_handoff::same_scheduler_t) const noexcept
      -> bool {
    return state != nullptr && state->same_scheduler;
  }

  [[nodiscard]] auto operator==(const manual_scheduler &) const noexcept
      -> bool = default;
};

template <typename would_block_t>
  requires(!std::same_as<would_block_t, void>)
class manual_scheduler<would_block_t> {
public:
  manual_scheduler_state *state{nullptr};

  using scheduler_concept = stdexec::scheduler_t;

  template <typename receiver_t> struct schedule_op final : manual_task {
    using operation_state_concept = stdexec::operation_state_t;

    manual_scheduler_state *state{nullptr};
    receiver_t receiver;

    schedule_op(manual_scheduler_state *state_ptr, receiver_t receiver_value)
        : state(state_ptr), receiver(std::move(receiver_value)) {}

    auto start() noexcept -> void { state->enqueue(this); }

    auto execute() noexcept -> void override {
      stdexec::set_value(std::move(receiver));
    }
  };

  struct schedule_sender {
    manual_scheduler_state *state{nullptr};

    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;

    template <typename receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) const noexcept
        -> schedule_op<receiver_t> {
      return schedule_op<receiver_t>{state, std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  template <typename receiver_t> struct try_schedule_op final : manual_task {
    using operation_state_concept = stdexec::operation_state_t;

    manual_scheduler_state *state{nullptr};
    receiver_t receiver;

    try_schedule_op(manual_scheduler_state *state_ptr,
                    receiver_t receiver_value)
        : state(state_ptr), receiver(std::move(receiver_value)) {}

    auto start() noexcept -> void {
      if (!state->allow_try_schedule) {
        stdexec::set_error(std::move(receiver), would_block_t{});
        return;
      }
      if (state->inline_try_schedule) {
        ++state->inline_try_schedule_calls;
        stdexec::set_value(std::move(receiver));
        return;
      }
      state->enqueue(this);
    }

    auto execute() noexcept -> void override {
      stdexec::set_value(std::move(receiver));
    }
  };

  struct try_schedule_sender {
    manual_scheduler_state *state{nullptr};

    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_error_t(would_block_t)>;

    template <typename receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) const noexcept
        -> try_schedule_op<receiver_t> {
      return try_schedule_op<receiver_t>{state, std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  [[nodiscard]] auto schedule() const noexcept -> schedule_sender {
    return {state};
  }

  [[nodiscard]] auto try_schedule() const noexcept -> try_schedule_sender {
    return {state};
  }

  [[nodiscard]] auto query(
      wh::core::detail::scheduler_handoff::same_scheduler_t) const noexcept
      -> bool {
    return state != nullptr && state->same_scheduler;
  }

  [[nodiscard]] auto operator==(const manual_scheduler &) const noexcept
      -> bool = default;
};

} // namespace wh::testing::helper
