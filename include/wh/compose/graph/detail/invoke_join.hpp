#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

template <typename receiver_t, typename derived_t, typename graph_scheduler_t>
class detail::invoke_runtime::run_state::join_base {
protected:
  struct pump_env {
    const wh::core::detail::any_resume_scheduler_t *graph_scheduler{nullptr};

    [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
        -> wh::core::detail::any_resume_scheduler_t {
      return *graph_scheduler;
    }

    template <typename cpo_t>
    [[nodiscard]] auto
    query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
        -> wh::core::detail::any_resume_scheduler_t {
      return *graph_scheduler;
    }

    template <typename cpo_t>
    [[nodiscard]] auto
    query(exec::get_completion_behavior_t<cpo_t>) const noexcept {
      return exec::completion_behavior::asynchronous_affine;
    }
  };

  struct outer_stop_callback {
    join_base *base{nullptr};

    auto operator()() const noexcept -> void {
      if (base == nullptr) {
        return;
      }
      base->stop_requested_.store(true, std::memory_order_release);
      base->request_pump();
    }
  };

  struct join_receiver {
    using receiver_concept = stdexec::receiver_t;

    join_base *base{nullptr};

    auto set_value() noexcept -> void { complete(); }

    auto set_stopped() noexcept -> void { complete(); }

    template <typename error_t>
    auto set_error(error_t &&) noexcept -> void { std::terminate(); }

    [[nodiscard]] auto get_env() const noexcept -> pump_env {
      return pump_env{std::addressof(base->graph_scheduler_env())};
    }

  private:
    auto complete() noexcept -> void {
      base->join_done_.store(true, std::memory_order_release);
      base->request_pump();
    }
  };

  using outer_env_t = stdexec::env_of_t<receiver_t>;
  using stored_outer_env_t = std::remove_cvref_t<outer_env_t>;
  using outer_stop_token_t = stdexec::stop_token_of_t<stored_outer_env_t>;
  using outer_stop_callback_t =
      stdexec::stop_callback_for_t<outer_stop_token_t, outer_stop_callback>;
  using join_scope_t = stdexec::counting_scope;
  using join_token_t =
      std::remove_cvref_t<decltype(std::declval<join_scope_t &>().get_token())>;
  using join_sender_t = decltype(std::declval<join_scope_t &>().join());
  using join_op_t = stdexec::connect_result_t<join_sender_t, join_receiver>;
  using pump_sender_t =
      decltype(stdexec::schedule(std::declval<const graph_scheduler_t &>()));

  static constexpr std::uint32_t no_slot_ =
      std::numeric_limits<std::uint32_t>::max();

  struct completion_slot {
    std::optional<node_frame> frame{};
    std::optional<wh::core::result<graph_value>> result{};
    std::uint32_t next{no_slot_};
  };

  struct finish_delivery {
    receiver_t receiver{};
    wh::core::result<graph_value> status{};

    auto complete() && noexcept -> void {
      stdexec::set_value(std::move(receiver), std::move(status));
    }
  };

  struct child_receiver {
    using receiver_concept = stdexec::receiver_t;

    join_base *base{nullptr};
    node_frame frame{};

    auto set_value(wh::core::result<graph_value> result) noexcept -> void {
      complete(std::move(result));
    }

    template <typename error_t>
    auto set_error(error_t &&) noexcept -> void {
      complete(wh::core::result<graph_value>::failure(
          wh::core::errc::internal_error));
    }

    auto set_stopped() noexcept -> void {
      complete(
          wh::core::result<graph_value>::failure(wh::core::errc::canceled));
    }

    [[nodiscard]] auto get_env() const noexcept -> pump_env {
      return pump_env{std::addressof(base->graph_scheduler_env())};
    }

  private:
    auto complete(wh::core::result<graph_value> result) noexcept -> void {
      base->enqueue_completion(std::move(frame), std::move(result));
      base->request_pump();
    }
  };

  using associated_child_sender_t = decltype(stdexec::associate(
      std::declval<graph_sender>(), std::declval<join_token_t>()));
  using child_op_t =
      stdexec::connect_result_t<associated_child_sender_t, child_receiver>;

  struct child_slot {
    alignas(child_op_t) std::byte storage_[sizeof(child_op_t)]{};
    bool engaged{false};
  };

  struct pump_receiver {
    using receiver_concept = stdexec::receiver_t;

    join_base *base{nullptr};

    auto set_value() noexcept -> void {
      base->destroy_pump();
      auto delivery = base->run_pump();
      if (delivery.has_value()) {
        std::move(*delivery).complete();
      }
    }

    template <typename error_t>
    auto set_error(error_t &&) noexcept -> void { std::terminate(); }

    auto set_stopped() noexcept -> void { std::terminate(); }

    [[nodiscard]] auto get_env() const noexcept -> pump_env {
      return pump_env{std::addressof(base->graph_scheduler_env())};
    }
  };

  using pump_op_t = stdexec::connect_result_t<pump_sender_t, pump_receiver>;

  [[nodiscard]] auto graph_scheduler_env() const noexcept
      -> const wh::core::detail::any_resume_scheduler_t & {
    return *graph_scheduler_env_;
  }

  [[nodiscard]] auto graph_scheduler() const noexcept
      -> const graph_scheduler_t & {
    return graph_scheduler_;
  }

  explicit join_base(
      const std::size_t node_count,
      wh::core::detail::any_resume_scheduler_t graph_scheduler_env,
      graph_scheduler_t graph_scheduler)
      : completion_slots_(node_count), child_slots_(node_count),
        graph_scheduler_env_(std::move(graph_scheduler_env)),
        graph_scheduler_(std::move(graph_scheduler)) {}

  ~join_base() {
    destroy_pump();
    destroy_children();
    destroy_join();
  }

  template <typename receiver_u>
  auto emplace_receiver(receiver_u &&receiver) -> void {
    receiver_.emplace(std::forward<receiver_u>(receiver));
    receiver_env_.emplace(stdexec::get_env(*receiver_));
  }

  auto bind_derived(derived_t *self) noexcept -> void { derived_ = self; }

  [[nodiscard]] auto receiver_env() const noexcept -> const stored_outer_env_t & {
    return *receiver_env_;
  }

  auto bind_outer_stop() noexcept -> void {
    if constexpr (!stdexec::unstoppable_token<outer_stop_token_t>) {
      auto stop_token = stdexec::get_stop_token(receiver_env());
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
        request_pump();
        return;
      }
      outer_stop_callback_.emplace(stop_token,
                                   outer_stop_callback{
                                       this});
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
        request_pump();
      }
    }
  }

  auto request_pump() noexcept -> void {
    pump_pending_.store(true, std::memory_order_release);

    bool expected = false;
    if (!pump_scheduled_.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
      return;
    }

    schedule_pump();
  }

  auto finish(wh::core::result<graph_value> status) noexcept -> void {
    if (finish_status_.has_value()) {
      return;
    }
    finish_status_.emplace(std::move(status));
    join_scope_.request_stop();
    join_scope_.close();
    start_join();
  }

  auto maybe_deliver_finish() noexcept -> void {
    if (!finish_status_.has_value() || result_delivered_ || running_async_ != 0U ||
        completion_head_.load(std::memory_order_acquire) != no_slot_ ||
        !join_done_.load(std::memory_order_acquire)) {
      return;
    }
    derived_->prepare_finish_delivery();
    destroy_join();
    result_delivered_ = true;
    outer_stop_callback_.reset();
    pending_delivery_.emplace(finish_delivery{
        .receiver = std::move(*receiver_),
        .status = std::move(*finish_status_),
    });
    receiver_.reset();
    receiver_env_.reset();
    finish_status_.reset();
  }

  auto poll_outer_stop() noexcept -> void {
    if (!stop_requested_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    finish(wh::core::result<graph_value>::failure(wh::core::errc::canceled));
  }

  auto start_child(graph_sender sender, node_frame &&frame)
      -> wh::core::result<void> {
    ++running_async_;
    const auto node_id = frame.node_id;
    try {
      auto &slot = child_slots_[node_id];
      if (slot.engaged) {
        std::terminate();
      }
      ::new (static_cast<void *>(child_ptr(node_id))) child_op_t(
          stdexec::connect(
              stdexec::associate(std::move(sender), join_scope_.get_token()),
              child_receiver{
                  .base = this,
                  .frame = std::move(frame),
              }));
      slot.engaged = true;
      stdexec::start(child_op(node_id));
      return {};
    } catch (...) {
      destroy_child(node_id);
      --running_async_;
      return wh::core::result<void>::failure(wh::core::errc::internal_error);
    }
  }

  template <typename release_fn_t, typename settle_fn_t>
  auto drain_completions(release_fn_t release_fn,
                         settle_fn_t settle_fn) noexcept -> void {
    auto head = completion_head_.exchange(no_slot_, std::memory_order_acquire);
    while (head != no_slot_) {
      const auto node_id = head;
      auto &slot = completion_slots_[node_id];
      const auto next = slot.next;
      slot.next = no_slot_;
      auto frame = std::move(*slot.frame);
      auto result = std::move(*slot.result);
      slot.frame.reset();
      slot.result.reset();
      head = next;

      if (running_async_ == 0U) {
        std::terminate();
      }
      --running_async_;
      destroy_child(node_id);

      if (finish_status_.has_value() || result_delivered_) {
        release_fn(frame);
        continue;
      }

      auto settled = settle_fn(std::move(frame), std::move(result));
      if (settled.has_error()) {
        finish(wh::core::result<graph_value>::failure(settled.error()));
      }
    }
  }

private:
  auto child_ptr(const std::uint32_t node_id) noexcept -> child_op_t * {
    return reinterpret_cast<child_op_t *>(child_slots_[node_id].storage_);
  }

  auto child_op(const std::uint32_t node_id) noexcept -> child_op_t & {
    return *std::launder(child_ptr(node_id));
  }

  auto destroy_child(const std::uint32_t node_id) noexcept -> void {
    auto &slot = child_slots_[node_id];
    if (!slot.engaged) {
      return;
    }
    std::destroy_at(std::launder(child_ptr(node_id)));
    slot.engaged = false;
  }

  auto destroy_children() noexcept -> void {
    for (std::uint32_t node_id = 0U;
         node_id < static_cast<std::uint32_t>(child_slots_.size()); ++node_id) {
      destroy_child(node_id);
    }
  }

  auto join_ptr() noexcept -> join_op_t * {
    return reinterpret_cast<join_op_t *>(join_storage_);
  }

  auto join_op() noexcept -> join_op_t & {
    return *std::launder(join_ptr());
  }

  auto pump_ptr() noexcept -> pump_op_t * {
    return reinterpret_cast<pump_op_t *>(pump_storage_);
  }

  auto pump_op() noexcept -> pump_op_t & {
    return *std::launder(pump_ptr());
  }

  auto destroy_pump() noexcept -> void {
    if (!pump_started_) {
      return;
    }
    std::destroy_at(std::launder(pump_ptr()));
    pump_started_ = false;
  }

  auto destroy_join() noexcept -> void {
    if (!join_started_) {
      return;
    }
    std::destroy_at(std::launder(join_ptr()));
    join_started_ = false;
  }

  auto schedule_pump() noexcept -> void {
    try {
      if (pump_started_) {
        std::terminate();
      }
      ::new (static_cast<void *>(pump_ptr())) pump_op_t(stdexec::connect(
          stdexec::schedule(graph_scheduler()),
          pump_receiver{
              .base = this,
          }));
      pump_started_ = true;
      stdexec::start(pump_op());
    } catch (...) { std::terminate(); }
  }

  auto run_pump() noexcept -> std::optional<finish_delivery> {
    for (;;) {
      pump_pending_.store(false, std::memory_order_release);
      derived_->pump();
      if (pending_delivery_.has_value()) {
        break;
      }
      if (!pump_pending_.load(std::memory_order_acquire)) {
        break;
      }
    }

    pump_scheduled_.store(false, std::memory_order_release);
    if (pending_delivery_.has_value()) {
      auto delivery = std::move(pending_delivery_);
      pending_delivery_.reset();
      return delivery;
    }
    if (!pump_pending_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    bool expected = false;
    if (pump_scheduled_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
      schedule_pump();
    }
    return std::nullopt;
  }

  auto start_join() noexcept -> void {
    if (join_started_) {
      return;
    }
    try {
      ::new (static_cast<void *>(join_ptr())) join_op_t(stdexec::connect(
          join_scope_.join(),
          join_receiver{
              .base = this,
          }));
      join_started_ = true;
      stdexec::start(join_op());
    } catch (...) { std::terminate(); }
  }

  auto enqueue_completion(node_frame &&frame,
                          wh::core::result<graph_value> &&result) noexcept
      -> void {
    auto &slot = completion_slots_[frame.node_id];
    if (slot.frame.has_value() || slot.result.has_value()) {
      std::terminate();
    }
    const auto node_id = frame.node_id;
    slot.frame.emplace(std::move(frame));
    slot.result.emplace(std::move(result));
    auto head = completion_head_.load(std::memory_order_relaxed);
    do {
      slot.next = head;
    } while (!completion_head_.compare_exchange_weak(
        head, node_id, std::memory_order_release,
        std::memory_order_relaxed));
  }

protected:
  std::optional<receiver_t> receiver_{};
  std::optional<stored_outer_env_t> receiver_env_{};
  join_scope_t join_scope_{};
  std::optional<outer_stop_callback_t> outer_stop_callback_{};
  std::vector<completion_slot> completion_slots_{};
  std::vector<child_slot> child_slots_{};
  std::atomic<std::uint32_t> completion_head_{no_slot_};
  std::atomic<bool> pump_scheduled_{false};
  std::atomic<bool> pump_pending_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> join_done_{false};
  alignas(pump_op_t) std::byte pump_storage_[sizeof(pump_op_t)]{};
  alignas(join_op_t) std::byte join_storage_[sizeof(join_op_t)]{};
  std::optional<wh::core::result<graph_value>> finish_status_{};
  std::optional<finish_delivery> pending_delivery_{};
  std::optional<wh::core::detail::any_resume_scheduler_t> graph_scheduler_env_{};
  [[no_unique_address]] graph_scheduler_t graph_scheduler_;
  derived_t *derived_{nullptr};
  std::size_t running_async_{0U};
  bool pump_started_{false};
  bool join_started_{false};
  bool result_delivered_{false};
};

} // namespace wh::compose
