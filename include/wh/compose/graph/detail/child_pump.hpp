// Defines the shared serial child-orchestration skeleton used by graph input
// and stream stages.
#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <exec/trampoline_scheduler.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/node/execution.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/detail/scheduled_resume_turn.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::compose::detail {

template <typename child_sender_t> struct child_pump_step {
  std::optional<child_sender_t> sender{};
  std::optional<wh::core::result<graph_value>> finish{};

  [[nodiscard]] static auto launch(child_sender_t sender_value) -> child_pump_step {
    child_pump_step step{};
    step.sender.emplace(std::move(sender_value));
    return step;
  }

  [[nodiscard]] static auto finish_with(wh::core::result<graph_value> status) -> child_pump_step {
    child_pump_step step{};
    step.finish.emplace(std::move(status));
    return step;
  }
};

template <typename stage_t, typename consume_fn_t, typename finish_fn_t> class child_batch_policy {
public:
  using child_sender_type = graph_sender;
  using completion_type = wh::core::result<graph_value>;

  child_batch_policy(std::vector<graph_sender> senders, stage_t stage, consume_fn_t consume,
                     finish_fn_t finish)
      : senders_(std::move(senders)), stage_(std::move(stage)), consume_(std::move(consume)),
        finish_(std::move(finish)) {}

  [[nodiscard]] auto next_step() -> wh::core::result<child_pump_step<graph_sender>> {
    if (next_index_ >= senders_.size()) {
      return child_pump_step<graph_sender>::finish_with(finish_(std::move(stage_)));
    }

    active_index_ = next_index_;
    return child_pump_step<graph_sender>::launch(std::move(senders_[next_index_++]));
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
  static constexpr std::size_t no_index_ = std::numeric_limits<std::size_t>::max();

  std::vector<graph_sender> senders_{};
  std::remove_cvref_t<stage_t> stage_;
  wh_no_unique_address std::remove_cvref_t<consume_fn_t> consume_;
  wh_no_unique_address std::remove_cvref_t<finish_fn_t> finish_;
  std::size_t next_index_{0U};
  std::size_t active_index_{no_index_};
};

template <typename stage_t, typename consume_fn_t, typename finish_fn_t>
[[nodiscard]] inline auto make_child_batch_policy(std::vector<graph_sender> senders,
                                                  stage_t &&stage, consume_fn_t &&consume,
                                                  finish_fn_t &&finish) {
  return child_batch_policy<std::remove_cvref_t<stage_t>, std::remove_cvref_t<consume_fn_t>,
                            std::remove_cvref_t<finish_fn_t>>{
      std::move(senders), std::forward<stage_t>(stage), std::forward<consume_fn_t>(consume),
      std::forward<finish_fn_t>(finish)};
}

template <typename stage_t, typename consume_fn_t, typename finish_fn_t>
[[nodiscard]] inline auto
make_child_batch_sender(std::vector<graph_sender> senders, stage_t &&stage, consume_fn_t &&consume,
                        finish_fn_t &&finish,
                        wh::core::detail::any_resume_scheduler_t graph_scheduler);

template <typename policy_t> class child_pump_sender {
  template <typename receiver_t> class operation {
    using self_t = operation;
    using policy_type = std::remove_cvref_t<policy_t>;
    using child_sender_t = typename policy_type::child_sender_type;
    using completion_t = typename policy_type::completion_type;
    using receiver_type = std::remove_cvref_t<receiver_t>;
    using graph_scheduler_t = wh::core::detail::any_resume_scheduler_t;
    using receiver_env_t =
        std::remove_cvref_t<decltype(stdexec::get_env(std::declval<const receiver_type &>()))>;
    friend class wh::core::detail::scheduled_resume_turn<self_t, graph_scheduler_t>;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      operation *self{nullptr};
      const receiver_env_t *env{nullptr};
      const graph_scheduler_t *graph_scheduler{nullptr};

      auto set_value(completion_t value) && noexcept -> void {
        self->finish_child(std::move(value));
      }

      template <typename error_t> auto set_error(error_t &&error) && noexcept -> void {
        self->finish_child(
            completion_t::failure(operation::map_async_error(std::forward<error_t>(error))));
      }

      auto set_stopped() && noexcept -> void {
        self->finish_child(completion_t::failure(wh::core::errc::canceled));
      }

      [[nodiscard]] auto get_env() const noexcept {
        return wh::core::detail::make_scheduler_env(*env, *graph_scheduler);
      }
    };

    using resumed_child_sender_t = decltype(stdexec::starts_on(
        exec::trampoline_scheduler{}, stdexec::starts_on(std::declval<const graph_scheduler_t &>(),
                                                         std::declval<child_sender_t>())));
    using child_op_t = stdexec::connect_result_t<resumed_child_sender_t, child_receiver>;

  public:
    using operation_state_concept = stdexec::operation_state_t;

    template <typename stored_receiver_t, typename stored_policy_t>
      requires std::constructible_from<policy_type, stored_policy_t &&>
    operation(stored_policy_t &&policy, const graph_scheduler_t &graph_scheduler,
              stored_receiver_t &&receiver)
        : receiver_(std::forward<stored_receiver_t>(receiver)),
          receiver_env_(stdexec::get_env(receiver_)), graph_scheduler_(graph_scheduler),
          policy_(std::forward<stored_policy_t>(policy)) {}

    operation(const operation &) = delete;
    auto operator=(const operation &) -> operation & = delete;
    operation(operation &&) = delete;
    auto operator=(operation &&) -> operation & = delete;

    ~operation() { cleanup(); }

    auto start() & noexcept -> void {
      try {
        if constexpr (requires(policy_type &policy) { policy.start(); }) {
          policy_.start();
        }
      } catch (...) {
        set_terminal(wh::core::result<graph_value>::failure(wh::core::map_current_exception()));
        arrive();
        return;
      }
      request_resume();
      arrive();
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

    [[nodiscard]] auto completed() const noexcept -> bool {
      return completed_.load(std::memory_order_acquire);
    }

    auto cleanup() noexcept -> void {
      resume_turn_.destroy();
      destroy_child();
      pending_status_.reset();
      pending_status_ready_.store(false, std::memory_order_release);
      if constexpr (requires(policy_type &policy) { policy.cleanup(); }) {
        policy_.cleanup();
      }
    }

    auto finish_child(completion_t status) noexcept -> void {
      if (completed()) {
        return;
      }
      wh_invariant(!pending_status_ready());
      pending_status_.emplace(std::move(status));
      pending_status_ready_.store(true, std::memory_order_release);
      request_resume();
      arrive();
    }

    auto set_terminal(wh::core::result<graph_value> status) noexcept -> void {
      if (terminal_.has_value()) {
        return;
      }
      terminal_.emplace(std::move(status));
      maybe_complete();
    }

    [[nodiscard]] auto pending_status_ready() const noexcept -> bool {
      return pending_status_ready_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto resume_turn_completed() const noexcept -> bool { return completed(); }

    auto request_resume() noexcept -> void { resume_turn_.request(this); }

    auto arrive() noexcept -> void {
      if (count_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        maybe_complete();
      }
    }

    auto resume_turn_arrive() noexcept -> void { arrive(); }

    auto resume_turn_add_ref() noexcept -> void { count_.fetch_add(1U, std::memory_order_relaxed); }

    auto resume_turn_schedule_error(const wh::core::error_code error) noexcept -> void {
      set_terminal(wh::core::result<graph_value>::failure(error));
    }

    auto resume_turn_run() noexcept -> void {
      resume();
      maybe_complete();
    }

    auto resume_turn_idle() noexcept -> void { maybe_complete(); }

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
      return terminal_.has_value() && !child_op_engaged_ && !pending_status_ready() &&
             !resume_turn_.running();
    }

    auto complete() noexcept -> void {
      if (!terminal_.has_value() || completed_.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      auto status = std::move(*terminal_);
      terminal_.reset();
      cleanup();
      stdexec::set_value(std::move(receiver_), std::move(status));
    }

    auto resume() noexcept -> void {
      while (!completed()) {
        if (pending_status_ready_.exchange(false, std::memory_order_acq_rel)) {
          destroy_child();
          auto completed = std::move(*pending_status_);
          pending_status_.reset();
          try {
            auto handled = policy_.handle_completion(std::move(completed));
            if (handled.has_value()) {
              set_terminal(std::move(*handled));
            }
          } catch (...) {
            set_terminal(wh::core::result<graph_value>::failure(wh::core::map_current_exception()));
          }
          continue;
        }

        if (terminal_.has_value()) {
          return;
        }

        if (child_op_engaged_) {
          return;
        }

        try {
          auto next = policy_.next_step();
          if (next.has_error()) {
            set_terminal(wh::core::result<graph_value>::failure(next.error()));
            continue;
          }

          auto step = std::move(next).value();
          const auto has_finish = step.finish.has_value();
          wh_invariant(step.sender.has_value() != has_finish);

          if (has_finish) {
            set_terminal(std::move(*step.finish));
            continue;
          }

          [[maybe_unused]] auto &child_op =
              child_op_.template construct_with<child_op_t>([&]() -> child_op_t {
                return stdexec::connect(
                    stdexec::starts_on(
                        exec::trampoline_scheduler{},
                        stdexec::starts_on(graph_scheduler_, std::move(*step.sender))),
                    child_receiver{this, std::addressof(receiver_env_),
                                   std::addressof(graph_scheduler_)});
              });
          child_op_engaged_ = true;
          count_.fetch_add(1U, std::memory_order_relaxed);
          stdexec::start(child_op_.template get<child_op_t>());
          return;
        } catch (...) {
          destroy_child();
          set_terminal(wh::core::result<graph_value>::failure(wh::core::map_current_exception()));
          continue;
        }
      }
    }

    auto destroy_child() noexcept -> void {
      if (!child_op_engaged_) {
        return;
      }
      child_op_.template destruct<child_op_t>();
      child_op_engaged_ = false;
    }

    receiver_type receiver_;
    receiver_env_t receiver_env_;
    graph_scheduler_t graph_scheduler_;
    wh_no_unique_address policy_type policy_;
    wh::core::detail::manual_storage<sizeof(child_op_t), alignof(child_op_t)> child_op_{};
    std::optional<completion_t> pending_status_{};
    std::optional<wh::core::result<graph_value>> terminal_{};
    std::atomic<std::size_t> count_{1U};
    std::atomic<bool> pending_status_ready_{false};
    std::atomic<bool> completed_{false};
    wh::core::detail::scheduled_resume_turn<self_t, graph_scheduler_t> resume_turn_{
        graph_scheduler_};
    bool child_op_engaged_{false};
  };

public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(wh::core::result<graph_value>)>;

  child_pump_sender(policy_t policy, wh::core::detail::any_resume_scheduler_t graph_scheduler)
      : policy_(std::move(policy)), graph_scheduler_(std::move(graph_scheduler)) {}

  child_pump_sender(const child_pump_sender &) = delete;
  auto operator=(const child_pump_sender &) -> child_pump_sender & = delete;
  child_pump_sender(child_pump_sender &&) noexcept = default;
  auto operator=(child_pump_sender &&) noexcept -> child_pump_sender & = default;

  template <typename self_t, stdexec::receiver_of<completion_signatures> receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, child_pump_sender> &&
             (!std::is_const_v<std::remove_reference_t<self_t>> ||
              std::copy_constructible<std::remove_cvref_t<policy_t>>)
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self, receiver_t receiver) {
    using operation_t = operation<std::remove_cvref_t<receiver_t>>;
    if constexpr (std::is_const_v<std::remove_reference_t<self_t>>) {
      return operation_t{self.policy_, self.graph_scheduler_, std::move(receiver)};
    } else {
      return operation_t{std::forward<self_t>(self).policy_, self.graph_scheduler_,
                         std::move(receiver)};
    }
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

private:
  wh_no_unique_address std::remove_cvref_t<policy_t> policy_;
  wh::core::detail::any_resume_scheduler_t graph_scheduler_;
};

template <typename policy_t>
[[nodiscard]] inline auto
make_child_pump_sender(policy_t &&policy,
                       wh::core::detail::any_resume_scheduler_t graph_scheduler) {
  return child_pump_sender<std::remove_cvref_t<policy_t>>{std::forward<policy_t>(policy),
                                                          std::move(graph_scheduler)};
}

template <typename stage_t, typename consume_fn_t, typename finish_fn_t>
[[nodiscard]] inline auto
make_child_batch_sender(std::vector<graph_sender> senders, stage_t &&stage, consume_fn_t &&consume,
                        finish_fn_t &&finish,
                        wh::core::detail::any_resume_scheduler_t graph_scheduler) {
  return make_child_pump_sender(make_child_batch_policy(std::move(senders),
                                                        std::forward<stage_t>(stage),
                                                        std::forward<consume_fn_t>(consume),
                                                        std::forward<finish_fn_t>(finish)),
                                std::move(graph_scheduler));
}

} // namespace wh::compose::detail
