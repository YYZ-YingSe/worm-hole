#pragma once

#include <memory>

#include "wh/compose/graph/graph.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/detail/child_completion_mailbox.hpp"
#include "wh/core/stdexec/detail/child_set.hpp"
#include "wh/core/stdexec/detail/receiver_completion.hpp"
#include "wh/core/stdexec/detail/scheduled_drive_loop.hpp"

namespace wh::compose {

template <typename receiver_t, typename derived_t, typename graph_scheduler_t>
class detail::invoke_runtime::invoke_join_base
    : public std::enable_shared_from_this<
          invoke_join_base<receiver_t, derived_t, graph_scheduler_t>>,
      public wh::core::detail::scheduled_drive_loop<
          invoke_join_base<receiver_t, derived_t, graph_scheduler_t>,
          graph_scheduler_t> {
  using drive_loop_t = wh::core::detail::scheduled_drive_loop<
      invoke_join_base<receiver_t, derived_t, graph_scheduler_t>,
      graph_scheduler_t>;
  friend drive_loop_t;
  friend class wh::core::detail::callback_guard<invoke_join_base>;

protected:
  struct graph_runtime_env {
    const graph_scheduler_t *graph_scheduler{nullptr};

    [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
        -> const graph_scheduler_t & {
      return *graph_scheduler;
    }

    template <typename cpo_t>
    [[nodiscard]] auto
    query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
        -> const graph_scheduler_t & {
      return *graph_scheduler;
    }

    template <typename cpo_t>
    [[nodiscard]] auto
    query(exec::get_completion_behavior_t<cpo_t>) const noexcept {
      return exec::completion_behavior::asynchronous_affine;
    }
  };

  struct outer_stop_callback {
    invoke_join_base *base{nullptr};

    auto operator()() const noexcept -> void {
      if (base == nullptr) {
        return;
      }
      auto scope = base->callbacks_.enter(base);
      base->stop_requested_.store(true, std::memory_order_release);
      base->request_drive();
    }
  };

  struct join_receiver {
    using receiver_concept = stdexec::receiver_t;

    invoke_join_base *base{nullptr};

    auto set_value() noexcept -> void { complete(); }

    auto set_stopped() noexcept -> void { complete(); }

    template <typename error_t> auto set_error(error_t &&) noexcept -> void {
      ::wh::core::contract_violation(
          ::wh::core::contract_kind::invariant,
          "invoke_join join receiver must not receive set_error");
    }

    [[nodiscard]] auto get_env() const noexcept -> graph_runtime_env {
      return graph_runtime_env{std::addressof(base->graph_scheduler())};
    }

  private:
    auto complete() noexcept -> void {
      auto scope = base->callbacks_.enter(base);
      base->join_completed_.store(true, std::memory_order_release);
      base->request_drive();
    }
  };

  using receiver_type = std::remove_cvref_t<receiver_t>;
  using receiver_env_t = std::remove_cvref_t<decltype(stdexec::get_env(
      std::declval<const receiver_type &>()))>;
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
  using final_completion_t =
      wh::core::detail::receiver_completion<receiver_type,
                                            wh::core::result<graph_value>>;

  struct completion_payload {
    node_frame frame{};
    wh::core::result<graph_value> result{};
  };

  struct child_receiver {
    using receiver_concept = stdexec::receiver_t;

    invoke_join_base *base{nullptr};
    node_frame frame{};

    auto set_value(wh::core::result<graph_value> result) noexcept -> void {
      complete(std::move(result));
    }

    template <typename error_t> auto set_error(error_t &&) noexcept -> void {
      complete(wh::core::result<graph_value>::failure(
          wh::core::errc::internal_error));
    }

    auto set_stopped() noexcept -> void {
      complete(
          wh::core::result<graph_value>::failure(wh::core::errc::canceled));
    }

    [[nodiscard]] auto get_env() const noexcept -> graph_runtime_env {
      return graph_runtime_env{std::addressof(base->graph_scheduler())};
    }

  private:
    auto complete(wh::core::result<graph_value> result) noexcept -> void {
      auto scope = base->callbacks_.enter(base);
      base->enqueue_completion(std::move(frame), std::move(result));
      base->request_drive();
    }
  };

  using associated_child_sender_t = decltype(stdexec::associate(
      std::declval<graph_sender>(), std::declval<join_token_t>()));
  using child_op_t =
      stdexec::connect_result_t<associated_child_sender_t, child_receiver>;
  using child_set_t = wh::core::detail::child_set<child_op_t>;
  using mailbox_t =
      wh::core::detail::child_completion_mailbox<completion_payload>;

  [[nodiscard]] auto graph_scheduler() const noexcept
      -> const graph_scheduler_t & {
    return this->scheduler();
  }

  template <typename receiver_u, typename graph_scheduler_u>
    requires std::constructible_from<graph_scheduler_t, graph_scheduler_u &&> &&
                 std::constructible_from<receiver_type, receiver_u &&>
  explicit invoke_join_base(const std::size_t node_count,
                            graph_scheduler_u &&graph_scheduler,
                            receiver_u &&receiver)
      : drive_loop_t(std::forward<graph_scheduler_u>(graph_scheduler)),
        receiver_(std::forward<receiver_u>(receiver)),
        receiver_env_(stdexec::get_env(receiver_)), children_(node_count),
        completions_(node_count) {}

  ~invoke_join_base() {
    children_.destroy_all();
    destroy_join();
  }

public:
  auto start() noexcept -> void {
    bind_outer_stop();
    request_drive();
  }

protected:
  auto bind_derived(derived_t *self) noexcept -> void { derived_ = self; }

  auto bind_outer_stop() noexcept -> void {
    if constexpr (!stdexec::unstoppable_token<outer_stop_token_t>) {
      auto stop_token = stdexec::get_stop_token(receiver_env_);
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
        request_drive();
        return;
      }
      outer_stop_callback_.emplace(stop_token, outer_stop_callback{this});
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
        request_drive();
      }
    }
  }

  [[nodiscard]] auto finished() const noexcept -> bool {
    return delivered_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto completion_pending() const noexcept -> bool {
    return pending_completion_.has_value();
  }

  [[nodiscard]] auto take_completion() noexcept
      -> std::optional<final_completion_t> {
    if (!pending_completion_.has_value()) {
      return std::nullopt;
    }
    auto completion = std::move(pending_completion_);
    pending_completion_.reset();
    return completion;
  }

  [[nodiscard]] auto acquire_owner_lifetime_guard() noexcept
      -> std::shared_ptr<invoke_join_base> {
    auto keepalive = this->weak_from_this().lock();
    if (!keepalive) {
      ::wh::core::contract_violation(
          ::wh::core::contract_kind::invariant,
          "invoke_join owner lifetime guard expired");
    }
    return keepalive;
  }

  auto request_drive() noexcept -> void { drive_loop_t::request_drive(); }

  auto on_callback_exit() noexcept -> void { request_drive(); }

  auto drive() noexcept -> void { derived_->drive(); }

  [[nodiscard]] auto callback_active() const noexcept -> bool {
    return callbacks_.active();
  }

  [[nodiscard]] auto completion_ready() const noexcept -> bool {
    return completions_.has_ready();
  }

  auto finish(wh::core::result<graph_value> status) noexcept -> void {
    if (finish_status_.has_value()) {
      return;
    }
    finish_status_.emplace(std::move(status));
    join_scope_.request_stop();
    join_scope_.close();
    start_join();
    request_drive();
  }

  auto maybe_deliver_finish() noexcept -> void {
    if (!finish_status_.has_value() || finished() ||
        children_.active_count() != 0U || completions_.has_ready() ||
        !join_completed_.load(std::memory_order_acquire)) {
      return;
    }

    auto status = std::move(*finish_status_);
    finish_status_.reset();
    deliver(std::move(status));
  }

  auto poll_outer_stop() noexcept -> void {
    if (!stop_requested_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    finish(wh::core::result<graph_value>::failure(wh::core::errc::canceled));
  }

  auto start_child(graph_sender sender, node_frame &&frame)
      -> wh::core::result<void> {
    const auto node_id = frame.node_id;
    return children_.start_child(node_id, [&](auto &slot) {
      slot.emplace_from(
          stdexec::connect,
          stdexec::associate(std::move(sender), join_scope_.get_token()),
          child_receiver{
              .base = this,
              .frame = std::move(frame),
          });
    });
  }

  template <typename release_fn_t, typename settle_fn_t>
  auto drain_completions(release_fn_t release_fn,
                         settle_fn_t settle_fn) noexcept -> void {
    completions_.drain(
        [&](const std::uint32_t slot_id, completion_payload payload) {
          children_.reclaim_child(slot_id);
          auto frame = std::move(payload.frame);
          auto result = std::move(payload.result);
          if (finish_status_.has_value() || finished()) {
            release_fn(frame);
            return;
          }

          auto settled = settle_fn(std::move(frame), std::move(result));
          if (settled.has_error()) {
            finish(wh::core::result<graph_value>::failure(settled.error()));
          }
        });
  }

  auto cleanup() noexcept -> void {
    if (derived_ != nullptr) {
      derived_->prepare_finish_delivery();
    }
    children_.destroy_all();
    destroy_join();
    outer_stop_callback_.reset();
    finish_status_.reset();
  }

private:
  auto drive_error(const wh::core::error_code error) noexcept -> void {
    deliver(wh::core::result<graph_value>::failure(error));
  }

  auto deliver(wh::core::result<graph_value> status) noexcept -> void {
    if (delivered_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    cleanup();
    pending_completion_.emplace(
        final_completion_t::set_value(std::move(receiver_), std::move(status)));
  }

  auto join_ptr() noexcept -> join_op_t * {
    return reinterpret_cast<join_op_t *>(join_storage_);
  }

  auto join_op() noexcept -> join_op_t & { return *std::launder(join_ptr()); }

  auto destroy_join() noexcept -> void {
    if (!join_started_) {
      return;
    }
    std::destroy_at(std::launder(join_ptr()));
    join_started_ = false;
  }

  auto start_join() noexcept -> void {
    if (join_started_) {
      return;
    }
    try {
      ::new (static_cast<void *>(join_ptr()))
          join_op_t(stdexec::connect(join_scope_.join(), join_receiver{
                                                             .base = this,
                                                         }));
      join_started_ = true;
      stdexec::start(join_op());
    } catch (...) {
      ::wh::core::contract_violation(::wh::core::contract_kind::invariant,
                                     "invoke_join join start must not throw");
    }
  }

  auto enqueue_completion(node_frame &&frame,
                          wh::core::result<graph_value> &&result) noexcept
      -> void {
#ifndef NDEBUG
    wh_invariant(
        completions_.publish(frame.node_id, completion_payload{
                                                .frame = std::move(frame),
                                                .result = std::move(result),
                                            }));
#else
    completions_.publish(frame.node_id, completion_payload{
                                            .frame = std::move(frame),
                                            .result = std::move(result),
                                        });
#endif
  }

protected:
  receiver_type receiver_;
  receiver_env_t receiver_env_;
  join_scope_t join_scope_{};
  std::optional<outer_stop_callback_t> outer_stop_callback_{};
  child_set_t children_{};
  mailbox_t completions_{};
  wh::core::detail::callback_guard<invoke_join_base> callbacks_{};
  std::atomic<bool> delivered_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> join_completed_{false};
  alignas(join_op_t) std::byte join_storage_[sizeof(join_op_t)]{};
  std::optional<wh::core::result<graph_value>> finish_status_{};
  std::optional<final_completion_t> pending_completion_{};
  derived_t *derived_{nullptr};
  bool join_started_{false};
};

} // namespace wh::compose
