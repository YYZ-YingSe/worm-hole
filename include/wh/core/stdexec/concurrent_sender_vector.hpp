// Defines a sender that runs a homogeneous sender vector with a bounded
// in-flight budget and collects each result in index order.
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace wh::core::detail {

template <typename result_t, stdexec::sender sender_t>
  requires wh::core::result_like<result_t>
class concurrent_sender_vector {
  template <typename receiver_t> class operation {
    using receiver_env_t =
        decltype(stdexec::get_env(std::declval<const receiver_t &>()));

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      operation *op{nullptr};
      std::size_t index{0U};
      receiver_env_t env_{};

      auto set_value(result_t status) && noexcept -> void {
        op->finish_child(index, std::move(status));
      }

      template <typename error_t>
      auto set_error(error_t &&) && noexcept -> void {
        op->finish_child(index,
                         result_t::failure(wh::core::errc::internal_error));
      }

      auto set_stopped() && noexcept -> void {
        op->finish_child(index, result_t::failure(wh::core::errc::canceled));
      }

      [[nodiscard]] auto get_env() const noexcept { return env_; }
    };

    using child_op_t = stdexec::connect_result_t<sender_t, child_receiver>;

  public:
    explicit operation(std::vector<sender_t> senders, std::size_t max_in_flight,
                       receiver_t receiver)
        : receiver_(std::move(receiver)), env_(stdexec::get_env(receiver_)),
          senders_(std::move(senders)), results_(senders_.size()),
          child_ops_(senders_.empty()
                         ? nullptr
                         : std::make_unique<
                               wh::core::detail::manual_lifetime_box<child_op_t>[]>(
                               senders_.size())),
          max_in_flight_(resolve_max_in_flight(max_in_flight, senders_.size())),
          remaining_(senders_.size()) {}

    auto start() & noexcept -> void {
      if (senders_.empty()) {
        stdexec::set_value(std::move(receiver_), std::vector<result_t>{});
        return;
      }
      drive();
    }

  private:
    [[nodiscard]] static auto resolve_max_in_flight(
        const std::size_t requested, const std::size_t total) noexcept
        -> std::size_t {
      if (total == 0U) {
        return 0U;
      }
      if (requested == 0U || requested >= total) {
        return total;
      }
      return requested;
    }

    auto drive() noexcept -> void {
      bool expected = false;
      if (!driving_.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return;
      }

      while (true) {
        while (active_.load(std::memory_order_acquire) < max_in_flight_) {
          const auto index =
              next_to_start_.fetch_add(1U, std::memory_order_acq_rel);
          if (index >= senders_.size()) {
            break;
          }

          active_.fetch_add(1U, std::memory_order_acq_rel);
          try {
            child_ops_[index].emplace_from(
                stdexec::connect, std::move(senders_[index]),
                child_receiver{this, index, env_});
          } catch (...) {
            finish_child(index,
                         result_t::failure(wh::core::errc::internal_error));
            continue;
          }
          stdexec::start(child_ops_[index].get());
        }

        driving_.store(false, std::memory_order_release);
        if (remaining_.load(std::memory_order_acquire) == 0U) {
          return;
        }
        if (active_.load(std::memory_order_acquire) >= max_in_flight_ ||
            next_to_start_.load(std::memory_order_acquire) >=
                senders_.size()) {
          return;
        }

        bool reacquire_drive = false;
        if (!driving_.compare_exchange_strong(reacquire_drive, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          return;
        }
      }
    }

    auto finish_child(const std::size_t index, result_t status) noexcept
        -> void {
      results_[index].emplace(std::move(status));
      active_.fetch_sub(1U, std::memory_order_acq_rel);

      if (remaining_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        complete();
        return;
      }

      drive();
    }

    auto complete() noexcept -> void {
      std::vector<result_t> results{};
      results.reserve(results_.size());
      for (auto &slot : results_) {
        if (slot.has_value()) {
          results.push_back(std::move(*slot));
          continue;
        }
        results.push_back(result_t::failure(wh::core::errc::internal_error));
      }
      stdexec::set_value(std::move(receiver_), std::move(results));
    }

    receiver_t receiver_;
    receiver_env_t env_;
    std::vector<sender_t> senders_{};
    std::vector<std::optional<result_t>> results_{};
    std::unique_ptr<wh::core::detail::manual_lifetime_box<child_op_t>[]>
        child_ops_{};
    std::size_t max_in_flight_{0U};
    std::atomic<std::size_t> next_to_start_{0U};
    std::atomic<std::size_t> active_{0U};
    std::atomic<std::size_t> remaining_{0U};
    std::atomic<bool> driving_{false};
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
    return stdexec::completion_signatures<
        stdexec::set_value_t(std::vector<result_t>)>{};
  }

private:
  std::vector<sender_t> senders_{};
  std::size_t max_in_flight_{0U};
};

template <typename result_t, stdexec::sender sender_t>
  requires wh::core::result_like<result_t>
[[nodiscard]] inline auto
make_concurrent_sender_vector(std::vector<sender_t> senders,
                              const std::size_t max_in_flight)
    -> concurrent_sender_vector<result_t, sender_t> {
  return concurrent_sender_vector<result_t, sender_t>{std::move(senders),
                                                      max_in_flight};
}

} // namespace wh::core::detail
