#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include <exec/trampoline_scheduler.hpp>

#include "wh/compose/graph/graph.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/stdexec/counting_scope.hpp"
#include "wh/core/stdexec/detail/scheduled_resume_turn.hpp"
#include "wh/core/stdexec/detail/slot_ready_list.hpp"
namespace wh::compose {

template <typename receiver_t, typename derived_t, typename graph_scheduler_t>
class detail::invoke_runtime::invoke_join_base {
protected:
  friend class wh::core::detail::scheduled_resume_turn<invoke_join_base, graph_scheduler_t>;

  // Protocol invariants:
  // 1. enter_terminal()/override_terminal() are the only ways to enter terminal
  //    mode.
  // 2. After terminal mode starts, derived runtimes may only drain and quiesce.
  // 3. Outer receiver completion is emitted from complete() only.
  // 4. count_ tracks outstanding async callbacks/ops that can still publish
  //    work into this operation; complete() is only legal once it reaches 0.
  struct graph_runtime_env {
    const graph_scheduler_t *graph_scheduler{nullptr};
    stdexec::inplace_stop_source *stop_source{nullptr};

    [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept -> const graph_scheduler_t & {
      return *graph_scheduler;
    }

    template <typename cpo_t>
    [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
        -> const graph_scheduler_t & {
      return *graph_scheduler;
    }

    [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
        -> stdexec::inplace_stop_token {
      return stop_source == nullptr ? stdexec::inplace_stop_token{} : stop_source->get_token();
    }

    template <typename cpo_t>
    [[nodiscard]] auto query(exec::get_completion_behavior_t<cpo_t>) const noexcept {
      return exec::completion_behavior::asynchronous_affine;
    }
  };

  struct outer_stop_callback {
    invoke_join_base *base{nullptr};

    auto operator()() const noexcept -> void {
      if (base == nullptr) {
        return;
      }
      auto *base_ptr = base;
      base_ptr->count_.fetch_add(1U, std::memory_order_relaxed);
      base_ptr->publish_stop_request();
      base_ptr->arrive();
    }
  };

  using receiver_type = std::remove_cvref_t<receiver_t>;
  using receiver_env_t =
      std::remove_cvref_t<decltype(stdexec::get_env(std::declval<const receiver_type &>()))>;
  using outer_env_t = stdexec::env_of_t<receiver_t>;
  using stored_outer_env_t = std::remove_cvref_t<outer_env_t>;
  using outer_stop_token_t = stdexec::stop_token_of_t<stored_outer_env_t>;
  using outer_stop_callback_t =
      stdexec::stop_callback_for_t<outer_stop_token_t, outer_stop_callback>;

  struct child_receiver {
    using receiver_concept = stdexec::receiver_t;

    invoke_join_base *base{nullptr};
    attempt_id attempt{};

    auto set_value(wh::core::result<graph_value> result) noexcept -> void {
      complete(std::move(result));
    }

    template <typename error_t> auto set_error(error_t &&error) noexcept -> void {
      complete(wh::core::result<graph_value>::failure(
          invoke_join_base::map_async_error(std::forward<error_t>(error))));
    }

    auto set_stopped() noexcept -> void {
      complete(wh::core::result<graph_value>::failure(wh::core::errc::canceled));
    }

    [[nodiscard]] auto get_env() const noexcept -> graph_runtime_env {
      return graph_runtime_env{
          .graph_scheduler = std::addressof(base->graph_scheduler_),
          .stop_source = std::addressof(base->child_stop_source_),
      };
    }

  private:
    auto complete(wh::core::result<graph_value> result) noexcept -> void {
      auto *base_ptr = base;
      base_ptr->publish_completion(attempt, std::move(result));
      base_ptr->arrive();
    }
  };

  struct child_join_receiver {
  public:
    using receiver_concept = stdexec::receiver_t;

    invoke_join_base *base{nullptr};

    auto set_value() && noexcept -> void {
      if (base != nullptr) {
        auto *base_ptr = base;
        base_ptr->signal_resume_edge();
        base_ptr->arrive();
      }
    }

    auto set_stopped() && noexcept -> void {
      if (base != nullptr) {
        auto *base_ptr = base;
        base_ptr->signal_resume_edge();
        base_ptr->arrive();
      }
    }

    template <typename error_t> auto set_error(error_t &&error) && noexcept -> void {
      if (base == nullptr) {
        return;
      }
      auto *base_ptr = base;
      base_ptr->publish_terminal_override(wh::core::result<graph_value>::failure(
          invoke_join_base::map_async_error(std::forward<error_t>(error))));
      base_ptr->arrive();
    }

    [[nodiscard]] auto get_env() const noexcept -> graph_runtime_env {
      return graph_runtime_env{
          .graph_scheduler = std::addressof(base->graph_scheduler_),
          .stop_source = std::addressof(base->child_stop_source_),
      };
    }
  };

  using scope_t = wh::core::detail::simple_counting_scope;
  using scope_token_t = decltype(std::declval<scope_t &>().get_token());
  using join_sender_t = decltype(std::declval<scope_t &>().join());
  using associated_sender_t =
      decltype(stdexec::associate(std::declval<graph_sender>(), std::declval<scope_token_t>()));
  using child_op_t = stdexec::connect_result_t<associated_sender_t, child_receiver>;
  using join_op_t = stdexec::connect_result_t<join_sender_t, child_join_receiver>;

  struct child_state {
    ~child_state() { reset(); }

    auto emplace(associated_sender_t sender, child_receiver receiver) -> void {
      ::new (static_cast<void *>(op()))
          child_op_t(stdexec::connect(std::move(sender), std::move(receiver)));
      engaged_ = true;
    }

    auto reset() noexcept -> void {
      if (!engaged_) {
        return;
      }
      op()->~child_op_t();
      engaged_ = false;
    }

    [[nodiscard]] auto get() noexcept -> child_op_t & { return *op(); }

    [[nodiscard]] auto op() noexcept -> child_op_t * {
      return std::launder(reinterpret_cast<child_op_t *>(op_storage_));
    }

    [[nodiscard]] auto op() const noexcept -> const child_op_t * {
      return std::launder(reinterpret_cast<const child_op_t *>(op_storage_));
    }

    alignas(child_op_t) std::byte op_storage_[sizeof(child_op_t)];
    std::optional<wh::core::result<graph_value>> completion{};
    bool budgeted{true};
    bool engaged_{false};
  };

  [[nodiscard]] auto graph_scheduler() const noexcept -> const graph_scheduler_t & {
    return graph_scheduler_;
  }

  template <typename receiver_u, typename graph_scheduler_u>
    requires std::constructible_from<graph_scheduler_t, graph_scheduler_u &&> &&
                 std::constructible_from<receiver_type, receiver_u &&>
  explicit invoke_join_base(const std::size_t node_count, graph_scheduler_u &&graph_scheduler,
                            receiver_u &&receiver)
      : receiver_(std::forward<receiver_u>(receiver)), receiver_env_(stdexec::get_env(receiver_)),
        child_states_(node_count == 0U ? nullptr : std::make_unique<child_state[]>(node_count)),
        child_state_count_(node_count), ready_children_(node_count),
        graph_scheduler_(std::forward<graph_scheduler_u>(graph_scheduler)),
        resume_turn_(graph_scheduler_) {}

public:
  using operation_state_concept = stdexec::operation_state_t;

  ~invoke_join_base() {
    outer_stop_callback_.reset();
    destroy_child_states();
    resume_turn_.destroy();
  }

  auto start() & noexcept -> void {
    bind_outer_stop();
    signal_resume_edge();
    arrive();
  }

protected:
  [[nodiscard]] auto derived() noexcept -> derived_t & { return static_cast<derived_t &>(*this); }

  [[nodiscard]] auto derived() const noexcept -> const derived_t & {
    return static_cast<const derived_t &>(*this);
  }

  auto bind_outer_stop() noexcept -> void {
    if constexpr (!stdexec::unstoppable_token<outer_stop_token_t>) {
      auto stop_token = stdexec::get_stop_token(receiver_env_);
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
        return;
      }
      try {
        outer_stop_callback_.emplace(stop_token, outer_stop_callback{this});
      } catch (...) {
        override_terminal(
            wh::core::result<graph_value>::failure(wh::core::map_current_exception()));
        return;
      }
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
      }
    }
  }

  auto schedule_resume_turn() noexcept -> void { resume_turn_.request(this); }

  auto signal_resume_edge() noexcept -> void {
    if (!resume_edge_.exchange(true, std::memory_order_acq_rel)) {
      schedule_resume_turn();
    }
  }

  [[nodiscard]] auto completed() const noexcept -> bool {
    return completed_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto resume_turn_completed() const noexcept -> bool { return completed(); }

  auto arrive() noexcept -> void {
    if (count_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
      maybe_complete();
    }
  }

  auto resume_turn_arrive() noexcept -> void { arrive(); }

  auto maybe_complete() noexcept -> void {
    if (completed()) {
      return;
    }
    if (count_.load(std::memory_order_acquire) != 0U || !should_complete()) {
      return;
    }
    complete();
  }

  [[nodiscard]] auto should_complete() const noexcept -> bool {
    return terminal_pending() && active_child_count() == 0U && !completion_ready() &&
           !resume_turn_.running();
  }

  [[nodiscard]] auto finished() const noexcept -> bool { return completed(); }

  [[nodiscard]] auto stop_requested() const noexcept -> bool {
    return stop_requested_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto completion_ready() const noexcept -> bool {
    return ready_children_.has_ready();
  }

  [[nodiscard]] auto active_child_count() const noexcept -> std::size_t {
    return active_child_count_;
  }

  [[nodiscard]] auto budgeted_child_count() const noexcept -> std::size_t {
    return budgeted_child_count_;
  }

  [[nodiscard]] auto terminal_pending() const noexcept -> bool {
    return terminal_status_.has_value();
  }

  auto enter_terminal(wh::core::result<graph_value> status) noexcept -> void {
    set_terminal(std::move(status));
    maybe_complete();
  }

  auto resume_turn_add_ref() noexcept -> void { count_.fetch_add(1U, std::memory_order_relaxed); }

  auto request_child_stop() noexcept -> void {
    if (!child_stop_source_.stop_requested()) {
      child_stop_source_.request_stop();
    }
  }

  auto reset_child_stop() noexcept -> void {
    std::destroy_at(std::addressof(child_stop_source_));
    std::construct_at(std::addressof(child_stop_source_));
  }

  auto resume_turn_schedule_error(const wh::core::error_code error) noexcept -> void {
    override_terminal(wh::core::result<graph_value>::failure(error));
  }

  auto resume_turn_run() noexcept -> void {
    resume_edge_.store(false, std::memory_order_release);
    drain_terminal_override();
    derived().resume();
    maybe_complete();
  }

  auto resume_turn_idle() noexcept -> void { maybe_complete(); }

  auto start_child(graph_sender sender, const attempt_id attempt,
                   const bool budgeted = true) -> wh::core::result<void> {
    wh_precondition(attempt.has_value());
    wh_precondition(attempt.slot < child_state_count_);

    auto &child = child_states_[attempt.slot];
    wh_invariant(!child.engaged_);

    try {
      child.emplace(stdexec::associate(std::move(sender), scope_.get_token()),
                    child_receiver{
                        .base = this,
                        .attempt = attempt,
                    });
      child.budgeted = budgeted;
      ++active_child_count_;
      if (budgeted) {
        ++budgeted_child_count_;
      }
      count_.fetch_add(1U, std::memory_order_relaxed);
      stdexec::start(child.get());
      return {};
    } catch (...) {
      child.reset();
      return wh::core::result<void>::failure(wh::core::map_current_exception());
    }
  }

  template <typename release_fn_t, typename settle_fn_t>
  auto drain_completions(release_fn_t release_fn, settle_fn_t settle_fn) noexcept -> void {
    ready_children_.drain([&](const std::uint32_t slot_id) {
      wh_precondition(slot_id < child_state_count_);
      auto &child = child_states_[slot_id];
      if (!child.completion.has_value()) {
        return;
      }

      auto result = std::move(*child.completion);
      child.completion.reset();
      settle_child(slot_id);
      const auto attempt = attempt_id{slot_id};

      if (terminal_pending() || completed()) {
        release_fn(attempt);
        return;
      }

      auto settled = settle_fn(attempt, std::move(result));
      if (settled.has_error()) {
        enter_terminal(wh::core::result<graph_value>::failure(settled.error()));
      }
    });
  }

private:
  template <typename error_t>
  [[nodiscard]] static auto map_async_error(error_t &&error) noexcept -> wh::core::error_code {
    if constexpr (std::same_as<std::remove_cvref_t<error_t>, wh::core::error_code>) {
      return std::forward<error_t>(error);
    } else if constexpr (std::same_as<std::remove_cvref_t<error_t>, std::exception_ptr>) {
      try {
        std::rethrow_exception(std::forward<error_t>(error));
      } catch (...) {
        return wh::core::map_current_exception();
      }
    } else {
      return wh::core::make_error(wh::core::errc::internal_error);
    }
  }

  auto close_children() noexcept -> void { scope_.close(); }

  auto set_terminal(wh::core::result<graph_value> status) noexcept -> void {
    if (terminal_pending()) {
      return;
    }
    terminal_status_ = std::move(status);
    close_children();
    if (!child_stop_source_.stop_requested()) {
      child_stop_source_.request_stop();
    }
    auto joined = start_join();
    if (joined.has_error()) {
      terminal_status_ = wh::core::result<graph_value>::failure(joined.error());
    }
    maybe_complete();
  }

  auto override_terminal(wh::core::result<graph_value> status) noexcept -> void {
    if (!terminal_pending()) {
      set_terminal(std::move(status));
      return;
    }
    terminal_status_ = std::move(status);
    maybe_complete();
  }

  auto start_join() -> wh::core::result<void> {
    if (join_op_engaged_) {
      return {};
    }

    try {
      ::new (static_cast<void *>(join_op()))
          join_op_t(stdexec::connect(scope_.join(), child_join_receiver{this}));
      join_op_engaged_ = true;
      count_.fetch_add(1U, std::memory_order_relaxed);
      stdexec::start(*join_op());
      return {};
    } catch (...) {
      return wh::core::result<void>::failure(wh::core::map_current_exception());
    }
  }

  auto settle_child(const std::uint32_t slot_id) noexcept -> void {
    wh_precondition(slot_id < child_state_count_);
    auto &child = child_states_[slot_id];
    wh_invariant(child.engaged_);
    wh_invariant(active_child_count_ != 0U);
    const auto budgeted = child.budgeted;
    child.reset();
    --active_child_count_;
    if (budgeted) {
      wh_invariant(budgeted_child_count_ != 0U);
      --budgeted_child_count_;
    }
  }

  auto publish_stop_request() noexcept -> void {
    if (completed()) {
      return;
    }
    const bool first_stop = !stop_requested_.exchange(true, std::memory_order_acq_rel);
    if (first_stop) {
      signal_resume_edge();
    }
  }

  auto publish_terminal_override(wh::core::result<graph_value> status) noexcept -> void {
    if (completed()) {
      return;
    }
    wh_invariant(!terminal_override_ready());
    terminal_override_.emplace(std::move(status));
    terminal_override_ready_.store(true, std::memory_order_release);
    signal_resume_edge();
  }

  [[nodiscard]] auto terminal_override_ready() const noexcept -> bool {
    return terminal_override_ready_.load(std::memory_order_acquire);
  }

  auto drain_terminal_override() noexcept -> void {
    if (!terminal_override_ready_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    wh_invariant(terminal_override_.has_value());
    auto status = std::move(*terminal_override_);
    terminal_override_.reset();
    override_terminal(std::move(status));
  }

  auto destroy_child_states() noexcept -> void {
    close_children();
    if (child_states_ != nullptr) {
      for (std::size_t index = 0U; index < child_state_count_; ++index) {
        auto &child = child_states_[index];
        child.reset();
        child.completion.reset();
      }
    }
    ready_children_.reset(0U);
    active_child_count_ = 0U;
    budgeted_child_count_ = 0U;
    if (join_op_engaged_) {
      join_op()->~join_op_t();
      join_op_engaged_ = false;
    }
    child_states_.reset();
    child_state_count_ = 0U;
  }

  [[nodiscard]] auto join_op() noexcept -> join_op_t * {
    return std::launder(reinterpret_cast<join_op_t *>(join_op_storage_));
  }

  [[nodiscard]] auto join_op() const noexcept -> const join_op_t * {
    return std::launder(reinterpret_cast<const join_op_t *>(join_op_storage_));
  }

  auto release_runtime_state() noexcept -> void {
    derived().prepare_finish_delivery();
    destroy_child_states();
    outer_stop_callback_.reset();
    terminal_override_.reset();
    terminal_override_ready_.store(false, std::memory_order_release);
    terminal_status_.reset();
  }

  auto complete() noexcept -> void {
    if (!terminal_pending() || completed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    auto status = std::move(*terminal_status_);
    release_runtime_state();
    stdexec::set_value(std::move(receiver_), std::move(status));
  }

  auto publish_completion(const attempt_id attempt, wh::core::result<graph_value> &&result) noexcept
      -> void {
    wh_precondition(attempt.has_value());
    wh_precondition(attempt.slot < child_state_count_);
    auto &child = child_states_[attempt.slot];
    wh_invariant(!child.completion.has_value());
    child.completion.emplace(std::move(result));
#ifndef NDEBUG
    wh_invariant(ready_children_.publish(attempt.slot));
#else
    ready_children_.publish(attempt.slot);
#endif
    signal_resume_edge();
  }

protected:
  receiver_type receiver_;
  receiver_env_t receiver_env_;
  std::optional<outer_stop_callback_t> outer_stop_callback_{};
  scope_t scope_{};
  std::unique_ptr<child_state[]> child_states_{};
  std::size_t child_state_count_{0U};
  wh::core::detail::slot_ready_list ready_children_{};
  std::size_t active_child_count_{0U};
  std::size_t budgeted_child_count_{0U};
  alignas(join_op_t) std::byte join_op_storage_[sizeof(join_op_t)];
  bool join_op_engaged_{false};
  graph_scheduler_t graph_scheduler_;
  stdexec::inplace_stop_source child_stop_source_{};
  wh::core::detail::scheduled_resume_turn<invoke_join_base, graph_scheduler_t> resume_turn_;
  std::atomic<std::size_t> count_{1U};
  std::atomic<bool> completed_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> resume_edge_{false};
  std::optional<wh::core::result<graph_value>> terminal_override_{};
  std::atomic<bool> terminal_override_ready_{false};
  std::optional<wh::core::result<graph_value>> terminal_status_{};
};

} // namespace wh::compose
