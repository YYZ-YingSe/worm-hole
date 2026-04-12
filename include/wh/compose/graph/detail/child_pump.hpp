// Defines the shared serial child-orchestration skeleton used by graph input
// and stream stages.
#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/detail/receiver_completion.hpp"
#include "wh/core/stdexec/detail/scheduled_drive_loop.hpp"
#include "wh/core/stdexec/detail/shared_operation_state.hpp"
#include "wh/core/stdexec/detail/single_completion_slot.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::compose::detail {

template <typename child_sender_t> struct child_pump_step {
  std::optional<child_sender_t> sender{};
  std::optional<wh::core::result<graph_value>> finish{};

  [[nodiscard]] static auto launch(child_sender_t sender_value)
      -> child_pump_step {
    child_pump_step step{};
    step.sender.emplace(std::move(sender_value));
    return step;
  }

  [[nodiscard]] static auto
  finish_with(wh::core::result<graph_value> status) -> child_pump_step {
    child_pump_step step{};
    step.finish.emplace(std::move(status));
    return step;
  }
};

template <typename stage_t, typename consume_fn_t, typename finish_fn_t>
class child_batch_policy {
public:
  using child_sender_type = graph_sender;
  using completion_type = wh::core::result<graph_value>;

  child_batch_policy(std::vector<graph_sender> senders, stage_t stage,
                     consume_fn_t consume, finish_fn_t finish)
      : senders_(std::move(senders)),
        stage_(std::move(stage)),
        consume_(std::move(consume)),
        finish_(std::move(finish)) {}

  [[nodiscard]] auto next_step()
      -> wh::core::result<child_pump_step<graph_sender>> {
    if (next_index_ >= senders_.size()) {
      return child_pump_step<graph_sender>::finish_with(
          finish_(std::move(stage_)));
    }

    active_index_ = next_index_;
    return child_pump_step<graph_sender>::launch(
        std::move(senders_[next_index_++]));
  }

  [[nodiscard]] auto handle_completion(completion_type current)
      -> std::optional<wh::core::result<graph_value>> {
    wh_invariant(active_index_ != no_index_);

    const auto index = active_index_;
    active_index_ = no_index_;
    auto consumed = consume_(stage_, index, std::move(current));
    if (consumed.has_error()) {
      return wh::core::result<graph_value>::failure(consumed.error());
    }
    return std::nullopt;
  }

  auto cleanup() noexcept -> void {
    active_index_ = no_index_;
    senders_.clear();
  }

private:
  static constexpr std::size_t no_index_ =
      std::numeric_limits<std::size_t>::max();

  std::vector<graph_sender> senders_{};
  std::remove_cvref_t<stage_t> stage_;
  wh_no_unique_address std::remove_cvref_t<consume_fn_t> consume_;
  wh_no_unique_address std::remove_cvref_t<finish_fn_t> finish_;
  std::size_t next_index_{0U};
  std::size_t active_index_{no_index_};
};

template <typename stage_t, typename consume_fn_t, typename finish_fn_t>
[[nodiscard]] inline auto make_child_batch_policy(
    std::vector<graph_sender> senders, stage_t &&stage, consume_fn_t &&consume,
    finish_fn_t &&finish) {
  return child_batch_policy<std::remove_cvref_t<stage_t>,
                            std::remove_cvref_t<consume_fn_t>,
                            std::remove_cvref_t<finish_fn_t>>{
      std::move(senders), std::forward<stage_t>(stage),
      std::forward<consume_fn_t>(consume),
      std::forward<finish_fn_t>(finish)};
}

template <typename stage_t, typename consume_fn_t, typename finish_fn_t>
[[nodiscard]] inline auto make_child_batch_sender(
    std::vector<graph_sender> senders, stage_t &&stage, consume_fn_t &&consume,
    finish_fn_t &&finish,
    wh::core::detail::any_resume_scheduler_t graph_scheduler);

template <typename policy_t> class child_pump_sender {
  template <typename receiver_t>
  class controller
      : public std::enable_shared_from_this<controller<receiver_t>>,
        public wh::core::detail::scheduled_drive_loop<
            controller<receiver_t>, wh::core::detail::any_resume_scheduler_t> {
    using drive_loop_t = wh::core::detail::scheduled_drive_loop<
        controller<receiver_t>, wh::core::detail::any_resume_scheduler_t>;
    friend drive_loop_t;
    friend class wh::core::detail::callback_guard<controller>;
    using policy_type = std::remove_cvref_t<policy_t>;
    using child_sender_t = typename policy_type::child_sender_type;
    using completion_t = typename policy_type::completion_type;
    using receiver_type = std::remove_cvref_t<receiver_t>;
    using receiver_env_t = std::remove_cvref_t<
        decltype(stdexec::get_env(std::declval<const receiver_type &>()))>;
    using final_completion_t =
        wh::core::detail::receiver_completion<receiver_type,
                                              wh::core::result<graph_value>>;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      controller *self{nullptr};
      const receiver_env_t *env{nullptr};
      const wh::core::detail::any_resume_scheduler_t *graph_scheduler{nullptr};

      auto set_value(completion_t value) && noexcept -> void {
        auto scope = self->callbacks_.enter(self);
        self->finish_child(std::move(value));
      }

      template <typename error_t>
      auto set_error(error_t &&error) && noexcept -> void {
        auto scope = self->callbacks_.enter(self);
        if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                                   wh::core::error_code>) {
          self->finish_child(
              completion_t::failure(std::forward<error_t>(error)));
        } else if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                                          std::exception_ptr>) {
          try {
            std::rethrow_exception(std::forward<error_t>(error));
          } catch (...) {
            self->finish_child(
                completion_t::failure(wh::core::map_current_exception()));
          }
        } else {
          self->finish_child(
              completion_t::failure(wh::core::errc::internal_error));
        }
      }

      auto set_stopped() && noexcept -> void {
        auto scope = self->callbacks_.enter(self);
        self->finish_child(completion_t::failure(wh::core::errc::canceled));
      }

      [[nodiscard]] auto get_env() const noexcept {
        return wh::core::detail::make_scheduler_env(*env, *graph_scheduler);
      }
    };

    using child_op_t =
        stdexec::connect_result_t<child_sender_t, child_receiver>;

  public:
    template <typename stored_receiver_t, typename stored_policy_t>
      requires std::constructible_from<policy_type, stored_policy_t &&>
    controller(stored_policy_t &&policy,
               const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
               stored_receiver_t &&receiver)
        : drive_loop_t(graph_scheduler),
          receiver_(std::forward<stored_receiver_t>(receiver)),
          receiver_env_(stdexec::get_env(receiver_)),
          policy_(std::forward<stored_policy_t>(policy)) {}

    auto start() noexcept -> void {
      try {
        if constexpr (requires(policy_type & policy) { policy.start(); }) {
          policy_.start();
        }
      } catch (...) {
        finish(wh::core::result<graph_value>::failure(
            wh::core::map_current_exception()));
        request_drive();
        return;
      }
      request_drive();
    }

  private:
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
        -> std::shared_ptr<controller> {
      auto keepalive = this->weak_from_this().lock();
      if (!keepalive) {
        ::wh::core::contract_violation(
            ::wh::core::contract_kind::invariant,
            "child_pump controller lifetime guard expired");
      }
      return keepalive;
    }

    auto on_callback_exit() noexcept -> void {
      if (completion_.ready()) {
        request_drive();
      }
    }

    auto request_drive() noexcept -> void { drive_loop_t::request_drive(); }

    auto drive() noexcept -> void {
      while (!finished()) {
        if (callbacks_.active()) {
          return;
        }

        if (auto current = completion_.take(); current.has_value()) {
          wh_invariant(child_in_flight_);
          child_op_.reset();
          child_in_flight_ = false;

          try {
            auto handled = policy_.handle_completion(std::move(*current));
            if (handled.has_value()) {
              finish(std::move(*handled));
              return;
            }
          } catch (...) {
            finish(wh::core::result<graph_value>::failure(
                wh::core::map_current_exception()));
            return;
          }
          continue;
        }

        if (child_in_flight_) {
          return;
        }

        if (auto next = run_next_step(); next.has_value()) {
          finish(std::move(*next));
          return;
        }

        if (completion_.ready()) {
          continue;
        }
        if (child_in_flight_) {
          return;
        }
        return;
      }
    }

    auto cleanup() noexcept -> void {
      completion_.reset();
      child_op_.reset();
      child_in_flight_ = false;
      if constexpr (requires(policy_type & policy) { policy.cleanup(); }) {
        policy_.cleanup();
      }
    }

    auto drive_error(const wh::core::error_code error) noexcept -> void {
      finish(wh::core::result<graph_value>::failure(error));
    }

    auto finish_child(completion_t status) noexcept -> void {
      if (finished()) {
        return;
      }
#ifndef NDEBUG
      wh_invariant(completion_.publish(std::move(status)));
#else
      completion_.publish(std::move(status));
#endif
      request_drive();
    }

    auto finish(wh::core::result<graph_value> status) noexcept -> void {
      if (delivered_.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      cleanup();
      pending_completion_.emplace(
          final_completion_t::set_value(std::move(receiver_),
                                        std::move(status)));
    }

    [[nodiscard]] auto run_next_step() noexcept
        -> std::optional<wh::core::result<graph_value>> {
      try {
        wh_invariant(!child_in_flight_ && !completion_.ready());

        auto next = policy_.next_step();
        if (next.has_error()) {
          return wh::core::result<graph_value>::failure(next.error());
        }

        auto step = std::move(next).value();
        const auto has_finish = step.finish.has_value();
        wh_invariant(step.sender.has_value() != has_finish);

        if (has_finish) {
          return std::move(*step.finish);
        }

        child_op_.emplace_from(stdexec::connect, std::move(*step.sender),
                               child_receiver{
                                   this,
                                   std::addressof(receiver_env_),
                                   std::addressof(this->scheduler()),
                               });
        child_in_flight_ = true;
        stdexec::start(child_op_.get());
      } catch (...) {
        child_op_.reset();
        child_in_flight_ = false;
        return wh::core::result<graph_value>::failure(
            wh::core::map_current_exception());
      }
      return std::nullopt;
    }

    receiver_type receiver_;
    receiver_env_t receiver_env_;
    wh_no_unique_address policy_type policy_;
    wh::core::detail::manual_lifetime_box<child_op_t> child_op_{};
    wh::core::detail::single_completion_slot<completion_t> completion_{};
    wh::core::detail::callback_guard<controller> callbacks_{};
    std::optional<final_completion_t> pending_completion_{};
    std::atomic<bool> delivered_{false};
    bool child_in_flight_{false};
  };

public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t(wh::core::result<graph_value>)>;

  child_pump_sender(
      policy_t policy, wh::core::detail::any_resume_scheduler_t graph_scheduler)
      : policy_(std::move(policy)),
        graph_scheduler_(std::move(graph_scheduler)) {}

  child_pump_sender(const child_pump_sender &) = delete;
  auto operator=(const child_pump_sender &) -> child_pump_sender & = delete;
  child_pump_sender(child_pump_sender &&) noexcept = default;
  auto operator=(child_pump_sender &&) noexcept -> child_pump_sender & =
      default;

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  [[nodiscard]] auto connect(receiver_t receiver) &&
      -> wh::core::detail::shared_operation_state<
          controller<std::remove_cvref_t<receiver_t>>> {
    using controller_t = controller<std::remove_cvref_t<receiver_t>>;
    return wh::core::detail::shared_operation_state<controller_t>{
        std::make_shared<controller_t>(std::move(policy_), graph_scheduler_,
                                       std::move(receiver))};
  }

private:
  wh_no_unique_address std::remove_cvref_t<policy_t> policy_;
  wh::core::detail::any_resume_scheduler_t graph_scheduler_;
};

template <typename policy_t>
[[nodiscard]] inline auto make_child_pump_sender(
    policy_t &&policy, wh::core::detail::any_resume_scheduler_t graph_scheduler) {
  return child_pump_sender<std::remove_cvref_t<policy_t>>{
      std::forward<policy_t>(policy), std::move(graph_scheduler)};
}

template <typename stage_t, typename consume_fn_t, typename finish_fn_t>
[[nodiscard]] inline auto make_child_batch_sender(
    std::vector<graph_sender> senders, stage_t &&stage, consume_fn_t &&consume,
    finish_fn_t &&finish,
    wh::core::detail::any_resume_scheduler_t graph_scheduler) {
  return make_child_pump_sender(
      make_child_batch_policy(std::move(senders), std::forward<stage_t>(stage),
                              std::forward<consume_fn_t>(consume),
                              std::forward<finish_fn_t>(finish)),
      std::move(graph_scheduler));
}

} // namespace wh::compose::detail
