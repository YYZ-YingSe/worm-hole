// Defines a sender that runs a homogeneous sender vector with a bounded
// in-flight budget and collects each result in index order.
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/detail/child_completion_mailbox.hpp"
#include "wh/core/stdexec/detail/child_set.hpp"
#include "wh/core/stdexec/detail/inline_drive_loop.hpp"
#include "wh/core/stdexec/detail/receiver_completion.hpp"

namespace wh::core::detail {

template <typename result_t, stdexec::sender sender_t>
  requires result_like<result_t>
class concurrent_sender_vector {
  template <typename receiver_t>
  class operation
      : public wh::core::detail::inline_drive_loop<operation<receiver_t>> {
    using drive_loop_t =
        wh::core::detail::inline_drive_loop<operation<receiver_t>>;
    friend drive_loop_t;
    friend class wh::core::detail::callback_guard<operation>;
    using receiver_env_t =
        decltype(stdexec::get_env(std::declval<const receiver_t &>()));
    using final_completion_t =
        wh::core::detail::receiver_completion<receiver_t,
                                              std::vector<result_t>>;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      operation *op{nullptr};
      std::uint32_t index{0U};
      receiver_env_t env_{};

      auto set_value(result_t status) && noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(index, std::move(status));
      }

      template <typename error_t>
      auto set_error(error_t &&) && noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(index,
                         result_t::failure(wh::core::errc::internal_error));
      }

      auto set_stopped() && noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(index, result_t::failure(wh::core::errc::canceled));
      }

      [[nodiscard]] auto get_env() const noexcept { return env_; }
    };

    using child_op_t = stdexec::connect_result_t<sender_t, child_receiver>;
    using child_set_t = wh::core::detail::child_set<child_op_t>;
    using mailbox_t = wh::core::detail::child_completion_mailbox<result_t>;

  public:
    explicit operation(std::vector<sender_t> senders, std::size_t max_in_flight,
                       receiver_t receiver)
        : receiver_(std::move(receiver)), env_(stdexec::get_env(receiver_)),
          senders_(std::move(senders)), results_(senders_.size()),
          children_(senders_.size()), completions_(senders_.size()),
          max_in_flight_(resolve_max_in_flight(max_in_flight, senders_.size())),
          remaining_(senders_.size()) {}

    auto start() & noexcept -> void {
      if (senders_.empty()) {
        delivered_.store(true, std::memory_order_release);
        pending_completion_.emplace(final_completion_t::set_value(
            std::move(receiver_), std::vector<result_t>{}));
        request_drive();
        return;
      }
      request_drive();
    }

  private:
    [[nodiscard]] static auto
    resolve_max_in_flight(const std::size_t requested,
                          const std::size_t total) noexcept -> std::size_t {
      if (total == 0U) {
        return 0U;
      }
      if (requested == 0U || requested >= total) {
        return total;
      }
      return requested;
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

    auto request_drive() noexcept -> void { drive_loop_t::request_drive(); }

    auto on_callback_exit() noexcept -> void {
      if (completions_.has_ready()) {
        request_drive();
      }
    }

    auto drive() noexcept -> void {
      while (!finished()) {
        if (callbacks_.active()) {
          return;
        }

        drain_completions();
        if (finished()) {
          return;
        }

        bool started_child = false;
        while (children_.active_count() < max_in_flight_) {
          const auto index = next_to_start_;
          ++next_to_start_;
          if (index >= senders_.size()) {
            break;
          }

          auto started = children_.start_child(
              static_cast<std::uint32_t>(index), [&](auto &slot) {
                slot.emplace_from(stdexec::connect, std::move(senders_[index]),
                                  child_receiver{
                                      this,
                                      static_cast<std::uint32_t>(index),
                                      env_,
                                  });
              });
          if (started.has_error()) {
            record_start_failure(index);
            continue;
          }
          started_child = true;
        }

        if (finished()) {
          return;
        }
        if (completions_.has_ready()) {
          continue;
        }
        if (children_.active_count() == 0U &&
            next_to_start_ >= senders_.size()) {
          complete();
          return;
        }
        if (!started_child) {
          return;
        }
      }
    }

    auto record_start_failure(const std::size_t index) noexcept -> void {
      wh_invariant(index < results_.size());
      wh_invariant(!results_[index].has_value());
      results_[index].emplace(
          result_t::failure(wh::core::errc::internal_error));
      wh_invariant(remaining_ != 0U);
      --remaining_;
    }

    auto finish_child(const std::uint32_t index, result_t status) noexcept
        -> void {
      if (finished()) {
        return;
      }
#ifndef NDEBUG
      wh_invariant(completions_.publish(index, std::move(status)));
#else
      completions_.publish(index, std::move(status));
#endif
      request_drive();
    }

    auto drain_completions() noexcept -> void {
      bool saw_completion = false;
      completions_.drain([&](const std::uint32_t index, result_t status) {
        children_.reclaim_child(index);
        saw_completion = true;
        wh_invariant(index < results_.size());
        wh_invariant(!results_[index].has_value());
        results_[index].emplace(std::move(status));
        wh_invariant(remaining_ != 0U);
        --remaining_;
      });
      if (saw_completion && remaining_ == 0U) {
        complete();
      }
    }

    auto complete() noexcept -> void {
      if (delivered_.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      std::vector<result_t> results{};
      results.reserve(results_.size());
      for (auto &slot : results_) {
        if (slot.has_value()) {
          results.push_back(std::move(*slot));
          continue;
        }
        results.push_back(result_t::failure(wh::core::errc::internal_error));
      }
      pending_completion_.emplace(final_completion_t::set_value(
          std::move(receiver_), std::move(results)));
    }

    receiver_t receiver_;
    receiver_env_t env_;
    std::vector<sender_t> senders_{};
    std::vector<std::optional<result_t>> results_{};
    child_set_t children_{};
    mailbox_t completions_{};
    std::size_t max_in_flight_{0U};
    std::size_t next_to_start_{0U};
    std::size_t remaining_{0U};
    wh::core::detail::callback_guard<operation> callbacks_{};
    std::optional<final_completion_t> pending_completion_{};
    std::atomic<bool> delivered_{false};
  };

public:
  using sender_concept = stdexec::sender_t;

  explicit concurrent_sender_vector(std::vector<sender_t> senders,
                                    const std::size_t max_in_flight)
      : senders_(std::move(senders)), max_in_flight_(max_in_flight) {}

  template <typename self_t, stdexec::receiver receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>,
                          concurrent_sender_vector> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>)
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                            receiver_t receiver) {
    return operation<receiver_t>{std::move(self).senders_,
                                 std::move(self).max_in_flight_,
                                 std::move(receiver)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>,
                          concurrent_sender_vector> &&
             (sizeof...(env_t) >= 1U)
  static consteval auto get_completion_signatures() {
    return stdexec::completion_signatures<stdexec::set_value_t(
        std::vector<result_t>)>{};
  }

private:
  std::vector<sender_t> senders_{};
  std::size_t max_in_flight_{0U};
};

template <typename result_t, stdexec::sender sender_t>
  requires result_like<result_t>
[[nodiscard]] inline auto
make_concurrent_sender_vector(std::vector<sender_t> senders,
                              const std::size_t max_in_flight)
    -> concurrent_sender_vector<result_t, sender_t> {
  return concurrent_sender_vector<result_t, sender_t>{std::move(senders),
                                                      max_in_flight};
}

} // namespace wh::core::detail
