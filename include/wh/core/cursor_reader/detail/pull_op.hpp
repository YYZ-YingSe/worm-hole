#pragma once

#include <cstddef>
#include <exception>
#include <atomic>
#include <type_traits>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/core/cursor_reader/detail/source.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"

namespace wh::core::cursor_reader_detail {

template <typename owner_t, typename source_t, typename result_t>
class pull_op final : public wh::core::detail::intrusive_enable_from_this<
                          pull_op<owner_t, source_t, result_t>> {
public:
  using self_t = pull_op<owner_t, source_t, result_t>;
  using owner_handle_t = wh::core::detail::intrusive_ptr<owner_t>;
  using scheduler_t = wh::core::detail::any_resume_scheduler_t;
  using sender_t = decltype(stdexec::starts_on(
      std::declval<scheduler_t>(), std::declval<source_t &>().read_async()));

  struct stop_env {
    stdexec::inplace_stop_token stop_token{};

    [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
        -> stdexec::inplace_stop_token {
      return stop_token;
    }
  };

  struct failure_state {
    wh::core::error_code code{wh::core::errc::internal_error};
  };

  struct stopped_state {};

  using completion_t = std::variant<result_t, failure_state, stopped_state>;

  struct receiver {
    using receiver_concept = stdexec::receiver_t;

    pull_op *self{nullptr};

    auto set_value(result_t status) noexcept -> void {
      self->finish_child(completion_t{std::move(status)});
    }

    template <typename error_t>
    auto set_error(error_t &&error) noexcept -> void {
      self->finish_child(completion_t{
          failure_state{map_error_code(std::forward<error_t>(error))}});
    }

    auto set_stopped() noexcept -> void {
      self->finish_child(completion_t{stopped_state{}});
    }

    [[nodiscard]] auto get_env() const noexcept -> stop_env {
      return stop_env{self->stop_source_.get_token()};
    }
  };

  using op_state_t = stdexec::connect_result_t<sender_t, receiver>;

  explicit pull_op(owner_t &owner) noexcept : owner_(owner.intrusive_from_this()) {}

  ~pull_op() { reset_child(); }

  auto start(source_t &source, scheduler_t scheduler) noexcept -> void {
    try {
      [[maybe_unused]] auto &child_op =
          child_op_.construct_with([&]() -> op_state_t {
        return stdexec::connect(
            stdexec::starts_on(std::move(scheduler), source.read_async()),
            receiver{this});
      });
      child_engaged_ = true;
      stdexec::start(child_op_.get());
    } catch (...) {
      finish_child(completion_t{
          failure_state{map_error_code(std::current_exception())}});
    }
  }

  auto request_stop() noexcept -> void {
    if (completed_.load(std::memory_order_acquire)) {
      return;
    }
    stop_source_.request_stop();
  }

private:
  auto reset_child() noexcept -> void {
    if (!child_engaged_) {
      return;
    }
    child_op_.destruct();
    child_engaged_ = false;
  }

  auto finish_child(completion_t completion) noexcept -> void {
    auto keepalive =
        static_cast<wh::core::detail::intrusive_enable_from_this<self_t> *>(
            this)
            ->intrusive_from_this();
    if (completed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    reset_child();

    if (auto *status = std::get_if<result_t>(&completion); status != nullptr) {
      owner_->finish_source_pull(this, std::move(*status), false);
    } else if (auto *failure = std::get_if<failure_state>(&completion);
               failure != nullptr) {
      owner_->finish_source_pull(this, owner_->async_failure(failure->code),
                                 true);
    } else {
      owner_->finish_source_pull_stopped(this);
    }
  }

  template <typename error_t>
  [[nodiscard]] static auto map_error_code(error_t &&error) noexcept
      -> wh::core::error_code {
    if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                               wh::core::error_code>) {
      return std::forward<error_t>(error);
    } else if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                                      std::exception_ptr>) {
      try {
        std::rethrow_exception(std::forward<error_t>(error));
      } catch (const std::exception &exception) {
        return wh::core::map_exception(exception);
      } catch (...) {
        return wh::core::errc::internal_error;
      }
    } else {
      return wh::core::errc::internal_error;
    }
  }

  owner_handle_t owner_{};
  stdexec::inplace_stop_source stop_source_{};
  wh::core::detail::manual_lifetime<op_state_t> child_op_{};
  bool child_engaged_{false};
  std::atomic<bool> completed_{false};
};

} // namespace wh::core::cursor_reader_detail
