#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/stdexec.hpp"
#include "wh/testing/sender_wait.hpp"

namespace wh::testing {

class static_thread_pool_handoff_context {
public:
  using fallback_scheduler_type = std::remove_cvref_t<
      decltype(std::declval<exec::static_thread_pool &>().get_scheduler())>;

  class scheduler {
  public:
    using scheduler_concept = stdexec::scheduler_t;

    struct domain;
    struct sender_env {
      static_thread_pool_handoff_context *context_{nullptr};

      template <typename cpo_t>
      [[nodiscard]] auto
      query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
          -> scheduler {
        return scheduler{context_};
      }

      template <typename cpo_t>
      [[nodiscard]] auto
      query(stdexec::get_completion_domain_t<cpo_t>) const noexcept -> domain {
        return {};
      }
    };

    struct try_schedule_blocked {};

    class schedule_sender;
    class handoff_sender;
    class try_handoff_sender;

    template <typename receiver_t> class async_schedule_operation {
    public:
      using operation_state_concept = stdexec::operation_state_t;

      struct fallback_receiver {
        using receiver_concept = stdexec::receiver_t;

        async_schedule_operation *self{nullptr};

        auto set_value() noexcept -> void { self->deliver(); }

        template <typename error_t>
        auto set_error(error_t &&error) noexcept -> void {
          tls_scope scope(self->context_);
          stdexec::set_error(std::move(self->receiver_),
                             std::forward<error_t>(error));
        }

        auto set_stopped() noexcept -> void {
          tls_scope scope(self->context_);
          stdexec::set_stopped(std::move(self->receiver_));
        }

        [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> {
          return {};
        }
      };

      using fallback_state_t =
          stdexec::connect_result_t<stdexec::schedule_result_t<fallback_scheduler_type>,
                                    fallback_receiver>;

      async_schedule_operation(scheduler scheduler_value, receiver_t receiver)
          : context_(scheduler_value.context_), receiver_(std::move(receiver)) {}

      auto start() & noexcept -> void {
        auto fallback_scheduler = context_->fallback_scheduler();
        fallback_state_.emplace(
            stdexec::connect(stdexec::schedule(fallback_scheduler),
                             fallback_receiver{this}));
        wh::testing::start_operation(*fallback_state_);
      }

    private:
      auto deliver() noexcept -> void {
        tls_scope scope(context_);
        auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver_));
        if constexpr (stdexec::unstoppable_token<decltype(stop_token)>) {
          stdexec::set_value(std::move(receiver_));
        } else if (stop_token.stop_requested()) {
          stdexec::set_stopped(std::move(receiver_));
        } else {
          stdexec::set_value(std::move(receiver_));
        }
      }

      static_thread_pool_handoff_context *context_{nullptr};
      receiver_t receiver_;
      std::optional<fallback_state_t> fallback_state_{};
    };

    template <typename receiver_t> class handoff_schedule_operation {
    public:
      using operation_state_concept = stdexec::operation_state_t;

      struct fallback_receiver {
        using receiver_concept = stdexec::receiver_t;

        handoff_schedule_operation *self{nullptr};

        auto set_value() noexcept -> void { self->deliver(); }

        template <typename error_t>
        auto set_error(error_t &&error) noexcept -> void {
          tls_scope scope(self->context_);
          stdexec::set_error(std::move(self->receiver_),
                             std::forward<error_t>(error));
        }

        auto set_stopped() noexcept -> void {
          tls_scope scope(self->context_);
          stdexec::set_stopped(std::move(self->receiver_));
        }

        [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> {
          return {};
        }
      };

      using fallback_state_t =
          stdexec::connect_result_t<stdexec::schedule_result_t<fallback_scheduler_type>,
                                    fallback_receiver>;

      handoff_schedule_operation(scheduler scheduler_value, receiver_t receiver)
          : context_(scheduler_value.context_), receiver_(std::move(receiver)) {}

      auto start() & noexcept -> void {
        if (context_->on_current_context()) {
          deliver();
          return;
        }

        auto fallback_scheduler = context_->fallback_scheduler();
        fallback_state_.emplace(
            stdexec::connect(stdexec::schedule(fallback_scheduler),
                             fallback_receiver{this}));
        wh::testing::start_operation(*fallback_state_);
      }

    private:
      auto deliver() noexcept -> void {
        tls_scope scope(context_);
        auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver_));
        if constexpr (stdexec::unstoppable_token<decltype(stop_token)>) {
          stdexec::set_value(std::move(receiver_));
        } else if (stop_token.stop_requested()) {
          stdexec::set_stopped(std::move(receiver_));
        } else {
          stdexec::set_value(std::move(receiver_));
        }
      }

      static_thread_pool_handoff_context *context_{nullptr};
      receiver_t receiver_;
      std::optional<fallback_state_t> fallback_state_{};
    };

    template <typename receiver_t> class try_schedule_operation {
    public:
      using operation_state_concept = stdexec::operation_state_t;

      try_schedule_operation(scheduler scheduler_value, receiver_t receiver)
          : context_(scheduler_value.context_), receiver_(std::move(receiver)) {}

      auto start() & noexcept -> void {
        auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver_));
        if constexpr (!stdexec::unstoppable_token<decltype(stop_token)>) {
          if (stop_token.stop_requested()) {
            stdexec::set_stopped(std::move(receiver_));
            return;
          }
        }

        if (context_->on_current_context()) {
          tls_scope scope(context_);
          stdexec::set_value(std::move(receiver_));
          return;
        }

        stdexec::set_error(std::move(receiver_), try_schedule_blocked{});
      }

    private:
      static_thread_pool_handoff_context *context_{nullptr};
      receiver_t receiver_;
    };

    class schedule_sender {
    public:
      using sender_concept = stdexec::sender_t;
      using completion_signatures =
          stdexec::completion_signatures<stdexec::set_value_t(),
                                         stdexec::set_stopped_t()>;

      explicit schedule_sender(scheduler scheduler_value) noexcept
          : context_(scheduler_value.context_) {}

      template <stdexec::receiver receiver_t>
      [[nodiscard]] auto connect(receiver_t receiver) const
          -> async_schedule_operation<receiver_t> {
        return async_schedule_operation<receiver_t>{scheduler{context_},
                                                    std::move(receiver)};
      }

      [[nodiscard]] auto get_env() const noexcept -> sender_env {
        return sender_env{context_};
      }

    private:
      static_thread_pool_handoff_context *context_{nullptr};
    };

    class handoff_sender {
    public:
      using sender_concept = stdexec::sender_t;
      using completion_signatures =
          stdexec::completion_signatures<stdexec::set_value_t(),
                                         stdexec::set_stopped_t()>;

      explicit handoff_sender(scheduler scheduler_value) noexcept
          : context_(scheduler_value.context_) {}

      template <stdexec::receiver receiver_t>
      [[nodiscard]] auto connect(receiver_t receiver) const
          -> handoff_schedule_operation<receiver_t> {
        return handoff_schedule_operation<receiver_t>{scheduler{context_},
                                                      std::move(receiver)};
      }

      [[nodiscard]] auto get_env() const noexcept -> sender_env {
        return sender_env{context_};
      }

    private:
      static_thread_pool_handoff_context *context_{nullptr};
    };

    class try_handoff_sender {
    public:
      using sender_concept = stdexec::sender_t;
      using completion_signatures =
          stdexec::completion_signatures<stdexec::set_value_t(),
                                         stdexec::set_error_t(try_schedule_blocked),
                                         stdexec::set_stopped_t()>;

      explicit try_handoff_sender(scheduler scheduler_value) noexcept
          : context_(scheduler_value.context_) {}

      template <stdexec::receiver receiver_t>
      [[nodiscard]] auto connect(receiver_t receiver) const
          -> try_schedule_operation<receiver_t> {
        return try_schedule_operation<receiver_t>{scheduler{context_},
                                                  std::move(receiver)};
      }

      [[nodiscard]] auto get_env() const noexcept -> sender_env {
        return sender_env{context_};
      }

    private:
      static_thread_pool_handoff_context *context_{nullptr};
    };

    struct domain : stdexec::default_domain {
      template <typename sender_t, typename env_t>
        requires wh::core::detail::scheduler_handoff::schedule_handoff_sender_like<
                     sender_t> &&
                 std::same_as<std::remove_cvref_t<decltype(
                                 std::declval<const std::remove_cvref_t<sender_t> &>()
                                      .target_scheduler())>,
                              scheduler>
      [[nodiscard]] auto transform_sender(stdexec::set_value_t, sender_t &&sender,
                                          const env_t &) const noexcept {
        return handoff_sender{sender.target_scheduler()};
      }

      template <typename sender_t, typename env_t>
        requires wh::core::detail::scheduler_handoff::
                     try_schedule_handoff_sender_like<sender_t> &&
                 std::same_as<std::remove_cvref_t<decltype(
                                 std::declval<const std::remove_cvref_t<sender_t> &>()
                                      .target_scheduler())>,
                              scheduler>
      [[nodiscard]] auto transform_sender(stdexec::set_value_t, sender_t &&sender,
                                          const env_t &) const noexcept {
        return try_handoff_sender{sender.target_scheduler()};
      }
    };

    [[nodiscard]] auto operator==(const scheduler &other) const noexcept -> bool {
      return context_ == other.context_;
    }

    [[nodiscard]] auto schedule() const noexcept -> schedule_sender {
      return schedule_sender{*this};
    }

    [[nodiscard]] auto try_schedule() const noexcept -> try_handoff_sender {
      return try_handoff_sender{*this};
    }

    [[nodiscard]] auto query(stdexec::get_domain_t) const noexcept -> domain {
      return {};
    }

    [[nodiscard]] auto
    query(stdexec::get_forward_progress_guarantee_t) const noexcept {
      return stdexec::get_forward_progress_guarantee(
          context_->fallback_scheduler());
    }

    template <typename cpo_t>
    [[nodiscard]] auto
    query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
        -> scheduler {
      return *this;
    }

    template <typename cpo_t>
    [[nodiscard]] auto
    query(stdexec::get_completion_domain_t<cpo_t>) const noexcept -> domain {
      return {};
    }

    [[nodiscard]] auto
    query(wh::core::detail::scheduler_handoff::same_scheduler_t) const
        noexcept -> bool {
      return context_->on_current_context();
    }

  private:
    friend class static_thread_pool_handoff_context;

    explicit scheduler(static_thread_pool_handoff_context *context) noexcept
        : context_(context) {}

    static_thread_pool_handoff_context *context_{nullptr};
  };

  explicit static_thread_pool_handoff_context(const std::uint32_t thread_count)
      : pool_(thread_count) {}

  [[nodiscard]] auto get_scheduler() noexcept -> scheduler {
    return scheduler{this};
  }

  [[nodiscard]] auto fallback_scheduler() noexcept -> fallback_scheduler_type {
    return pool_.get_scheduler();
  }

  class tls_scope {
  public:
    explicit tls_scope(static_thread_pool_handoff_context *context) noexcept
        : previous_(current_context_) {
      current_context_ = context;
    }

    ~tls_scope() { current_context_ = previous_; }

    tls_scope(const tls_scope &) = delete;
    auto operator=(const tls_scope &) -> tls_scope & = delete;

  private:
    static_thread_pool_handoff_context *previous_{nullptr};
  };

  [[nodiscard]] auto on_current_context() const noexcept -> bool {
    return current_context_ == this;
  }

private:
  exec::static_thread_pool pool_;
  inline static thread_local static_thread_pool_handoff_context
      *current_context_ = nullptr;
};

} // namespace wh::testing
