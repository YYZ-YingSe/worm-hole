#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <optional>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/intrusive_ptr.hpp"
#include "wh/core/cursor_reader/detail/shared_state.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"

namespace wh::core::cursor_reader_detail {

template <wh::core::cursor_reader_source source_t, typename policy_t,
          typename receiver_t>
  requires wh::core::cursor_reader_detail::policy_for<source_t, policy_t>
struct read_operation final
    : async_waiter_base<typename policy_t::result_type> {
  using result_type = typename policy_t::result_type;
  using shared_state_t = shared_state<source_t, policy_t>;
  using async_waiter_t = async_waiter_base<result_type>;
  using scheduler_t =
      wh::core::detail::resume_scheduler_t<stdexec::env_of_t<receiver_t>>;
  using stop_token_t = stdexec::stop_token_of_t<stdexec::env_of_t<receiver_t>>;
  using handoff_sender_t = stdexec::schedule_result_t<scheduler_t>;

  struct stop_callback;
  struct handoff_receiver {
    using receiver_concept = stdexec::receiver_t;

    read_operation *self{nullptr};

    auto set_value() noexcept -> void {
      self->reset_handoff();
      self->deliver();
    }

    template <typename error_t>
    auto set_error(error_t &&error) noexcept -> void {
      self->reset_handoff();
      self->deliver_error(std::forward<error_t>(error));
    }

    auto set_stopped() noexcept -> void {
      self->reset_handoff();
      self->deliver_stopped();
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  using stop_callback_t =
      stdexec::stop_callback_for_t<stop_token_t, stop_callback>;
  using handoff_op_t =
      stdexec::connect_result_t<handoff_sender_t, handoff_receiver>;

  struct stop_callback {
    read_operation *self{nullptr};

    auto operator()() const noexcept -> void {
      self->stop_requested_.store(true, std::memory_order_release);
      self->cancel_wait();
    }
  };

  wh::core::detail::intrusive_ptr<shared_state_t> state_{};
  std::size_t reader_index{0U};
  bool released{true};
  receiver_t receiver_;
  scheduler_t scheduler_;
  wh::core::detail::manual_lifetime<handoff_op_t> handoff_op_{};
  bool handoff_engaged_{false};
  std::optional<stop_callback_t> on_stop_{};
  std::atomic<bool> stop_requested_{false};
  bool stopped_{false};
  std::atomic<std::uint8_t> state_bits_{0U};

  template <typename receiver_u>
    requires std::constructible_from<receiver_t, receiver_u &&>
  read_operation(wh::core::detail::intrusive_ptr<shared_state_t> state,
                 const std::size_t reader_index_value,
                 const bool released_value, receiver_u &&receiver)
      : state_(std::move(state)), reader_index(reader_index_value),
        released(released_value), receiver_(std::forward<receiver_u>(receiver)),
        scheduler_(
            wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(
                stdexec::get_env(receiver_))) {
    static constexpr typename async_waiter_t::ops_type ops{
        [](async_waiter_t *base) noexcept {
          static_cast<read_operation *>(base)->complete_ready();
        }};
    this->ops = &ops;
  }

  read_operation(const read_operation &) = delete;
  auto operator=(const read_operation &) -> read_operation & = delete;
  read_operation(read_operation &&) = delete;
  auto operator=(read_operation &&) -> read_operation & = delete;
  ~read_operation() {
    on_stop_.reset();
    reset_handoff();
  }

  using operation_state_concept = stdexec::operation_state_t;

  auto start() & noexcept -> void {
    if (!state_) {
      stdexec::set_value(std::move(receiver_), policy_t::internal_result());
      return;
    }
    if (released) {
      stdexec::set_value(std::move(receiver_), policy_t::closed_result());
      return;
    }

    auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver_));
    if (stop_token.stop_requested()) {
      stdexec::set_stopped(std::move(receiver_));
      return;
    }

    if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
      try {
        on_stop_.emplace(stop_token, stop_callback{this});
      } catch (...) {
        stdexec::set_error(std::move(receiver_), std::current_exception());
        return;
      }
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
      }
    }

    auto ticket = state_->register_async_waiter(reader_index, this);
    if (ticket.ready.has_value()) {
      this->store_ready(std::move(*ticket.ready));
      if (!claim_completion()) {
        return;
      }
      begin_completion();
      return;
    }

    if (stop_requested_.load(std::memory_order_acquire)) {
      cancel_wait();
      if (has_claimed_completion()) {
        return;
      }
    }

    if (ticket.start_pull) {
      state_->start_async_pull(
          wh::core::detail::erase_resume_scheduler(scheduler_));
    }
  }

private:
  static constexpr std::uint8_t claimed_bit_ = 0x1U;
  static constexpr std::uint8_t delivering_bit_ = 0x2U;

  [[nodiscard]] auto has_claimed_completion() const noexcept -> bool {
    return (state_bits_.load(std::memory_order_acquire) & claimed_bit_) != 0U;
  }

  [[nodiscard]] auto claim_completion() noexcept -> bool {
    auto state_bits = state_bits_.load(std::memory_order_acquire);
    for (;;) {
      if ((state_bits & claimed_bit_) != 0U) {
        return false;
      }
      const auto updated = static_cast<std::uint8_t>(state_bits | claimed_bit_);
      if (state_bits_.compare_exchange_weak(state_bits, updated,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return true;
      }
    }
  }

  [[nodiscard]] auto start_delivery() noexcept -> bool {
    return (state_bits_.fetch_or(delivering_bit_, std::memory_order_acq_rel) &
            delivering_bit_) == 0U;
  }

  auto release_stop() noexcept -> void { on_stop_.reset(); }

  auto reset_handoff() noexcept -> void {
    if (!handoff_engaged_) {
      return;
    }
    handoff_op_.destruct();
    handoff_engaged_ = false;
  }

  auto cancel_wait() noexcept -> void {
    if (!state_ || !this->waiting_registered() || has_claimed_completion()) {
      return;
    }
    if (!state_->remove_async_waiter(reader_index, this)) {
      return;
    }
    if (!claim_completion()) {
      return;
    }
    stopped_ = true;
    begin_completion();
  }

  auto complete_ready() noexcept -> void {
    if (!claim_completion()) {
      return;
    }
    begin_completion();
  }

  [[nodiscard]] auto is_same_scheduler() const noexcept -> bool {
    return wh::core::detail::scheduler_handoff::same_scheduler(scheduler_);
  }

  auto begin_completion() noexcept -> void {
    if (is_same_scheduler()) {
      deliver();
      return;
    }
    try {
      [[maybe_unused]] auto &handoff_op =
          handoff_op_.construct_with([&]() -> handoff_op_t {
        return stdexec::connect(stdexec::schedule(scheduler_),
                                handoff_receiver{this});
      });
      handoff_engaged_ = true;
      stdexec::start(handoff_op_.get());
    } catch (...) {
      reset_handoff();
      deliver_error(std::current_exception());
    }
  }

  auto deliver() noexcept -> void {
    if (!start_delivery()) {
      return;
    }
    release_stop();
    if (stopped_) {
      stdexec::set_stopped(std::move(receiver_));
      return;
    }
    stdexec::set_value(std::move(receiver_), this->take_ready());
  }

  template <typename error_t> auto deliver_error(error_t &&error) noexcept
      -> void {
    if (!start_delivery()) {
      return;
    }
    release_stop();
    stdexec::set_error(std::move(receiver_),
                       wh::core::cursor_reader_detail::to_exception_ptr(
                           std::forward<error_t>(error)));
  }

  auto deliver_stopped() noexcept -> void {
    if (!start_delivery()) {
      return;
    }
    release_stop();
    stdexec::set_stopped(std::move(receiver_));
  }
};

template <wh::core::cursor_reader_source source_t, typename policy_t>
  requires wh::core::cursor_reader_detail::policy_for<source_t, policy_t>
struct read_sender {
  using result_type = typename policy_t::result_type;
  using sender_concept = stdexec::sender_t;
  using is_sender = void;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(result_type),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;

  wh::core::detail::intrusive_ptr<shared_state<source_t, policy_t>> state_{};
  std::size_t reader_index{0U};
  bool released{true};

  template <stdexec::receiver_of<completion_signatures> receiver_t>
    requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
  [[nodiscard]] auto connect(receiver_t receiver) && -> read_operation<
      source_t, policy_t, std::remove_cvref_t<receiver_t>> {
    return read_operation<source_t, policy_t, std::remove_cvref_t<receiver_t>>{
        std::move(state_), reader_index, released, std::move(receiver)};
  }

  template <stdexec::receiver_of<completion_signatures> receiver_t>
    requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
  [[nodiscard]] auto connect(receiver_t receiver) const
      & -> read_operation<source_t, policy_t, std::remove_cvref_t<receiver_t>> {
    return read_operation<source_t, policy_t, std::remove_cvref_t<receiver_t>>{
        state_, reader_index, released, std::move(receiver)};
  }

  [[nodiscard]] auto get_env() const noexcept
      -> wh::core::detail::async_completion_env {
    return {};
  }
};

} // namespace wh::core::cursor_reader_detail
