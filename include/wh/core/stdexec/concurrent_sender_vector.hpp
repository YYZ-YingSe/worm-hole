// Defines a bounded homogeneous sender fan-out that keeps a fixed in-flight
// budget and collects child results in index order.
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exec/trampoline_scheduler.hpp>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"

namespace wh::core::detail {

template <typename result_t, stdexec::sender sender_t>
  requires result_like<result_t>
class concurrent_sender_vector {
  template <typename receiver_t> class operation {
    using receiver_env_t =
        decltype(stdexec::get_env(std::declval<const receiver_t &>()));
    using outer_stop_token_t = stdexec::stop_token_of_t<receiver_env_t>;
    using stop_token_env_t =
        decltype(stdexec::prop{stdexec::get_stop_token, stdexec::inplace_stop_token{}});

    enum class control_state : std::uint8_t {
      started,
      stopped,
    };

    struct stop_callback {
      operation *op{nullptr};

      auto operator()() const noexcept -> void { op->request_stop(); }
    };

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      operation *op{nullptr};
      std::uint32_t index{0U};
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

    enum class start_result : std::uint8_t {
      started,
      advance,
      exhausted,
      completed,
    };

    using child_sender_t =
        decltype(stdexec::starts_on(exec::trampoline_scheduler{},
                                    stdexec::write_env(
                                        std::declval<sender_t>(),
                                        std::declval<stop_token_env_t>())));
    using child_op_t =
        stdexec::connect_result_t<child_sender_t, child_receiver>;
    using stop_callback_t =
        stdexec::stop_callback_for_t<outer_stop_token_t, stop_callback>;

  public:
    explicit operation(std::vector<sender_t> senders,
                       const std::size_t max_in_flight, receiver_t receiver)
        : receiver_(std::move(receiver)), env_(stdexec::get_env(receiver_)),
          senders_(std::move(senders)),
          results_(allocate_results(senders_.size())),
          result_engaged_(senders_.size(), 0U),
          child_ops_(allocate_child_ops(senders_.size())),
          child_engaged_(senders_.size(), 0U),
          max_in_flight_(resolve_max_in_flight(max_in_flight, senders_.size())),
          remaining_(senders_.size()) {}

    operation(const operation &) = delete;
    auto operator=(const operation &) -> operation & = delete;
    operation(operation &&) = delete;
    auto operator=(operation &&) -> operation & = delete;

    ~operation() {
      on_stop_.reset();
      destroy_results();
      destroy_children();
    }

    auto start() & noexcept -> void {
      if (senders_.empty()) {
        completed_.store(true, std::memory_order_release);
        stdexec::set_value(std::move(receiver_), std::vector<result_t>{});
        return;
      }

      bind_stop();

      for (std::size_t slot = 0U; slot < max_in_flight_; ++slot) {
        if (state_.load(std::memory_order_acquire) != control_state::started) {
          break;
        }
        const auto started = start_next();
        if (started == start_result::completed) {
          arrive();
          return;
        }
        if (started == start_result::exhausted) {
          break;
        }
      }

      arrive();
    }

  private:
    auto bind_stop() noexcept -> void {
      if constexpr (!stdexec::unstoppable_token<outer_stop_token_t>) {
        auto stop_token = stdexec::get_stop_token(env_);
        if (stop_token.stop_requested()) {
          mark_stopped();
          return;
        }
        try {
          on_stop_.emplace(stop_token, stop_callback{this});
        } catch (...) {
          mark_stopped();
          return;
        }
        if (stop_token.stop_requested()) {
          mark_stopped();
        }
      }
    }

    auto mark_stopped() noexcept -> void {
      auto expected = control_state::started;
      if (state_.compare_exchange_strong(expected, control_state::stopped,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        stop_source_.request_stop();
      }
    }

    auto request_stop() noexcept -> void {
      count_.fetch_add(1U, std::memory_order_relaxed);
      mark_stopped();
      arrive();
    }

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

    [[nodiscard]] static auto
    allocate_child_ops(const std::size_t count)
        -> std::unique_ptr<wh::core::detail::manual_lifetime<child_op_t>[]> {
      if (count == 0U) {
        return {};
      }
      return std::make_unique<wh::core::detail::manual_lifetime<child_op_t>[]>(
          count);
    }

    [[nodiscard]] static auto
    allocate_results(const std::size_t count)
        -> std::unique_ptr<wh::core::detail::manual_lifetime<result_t>[]> {
      if (count == 0U) {
        return {};
      }
      return std::make_unique<wh::core::detail::manual_lifetime<result_t>[]>(
          count);
    }

    auto destroy_children() noexcept -> void {
      if (!child_ops_) {
        return;
      }
      for (std::size_t index = 0U; index < child_engaged_.size(); ++index) {
        if (child_engaged_[index] == 0U) {
          continue;
        }
        child_ops_[index].destruct();
        child_engaged_[index] = 0U;
      }
    }

    auto destroy_results() noexcept -> void {
      if (!results_) {
        return;
      }
      for (std::size_t index = 0U; index < result_engaged_.size(); ++index) {
        if (result_engaged_[index] == 0U) {
          continue;
        }
        results_[index].destruct();
        result_engaged_[index] = 0U;
      }
    }

    auto arrive() noexcept -> void {
      if (count_.fetch_sub(1U, std::memory_order_acq_rel) == 1U &&
          should_complete()) {
        complete();
      }
    }

    [[nodiscard]] auto should_complete() const noexcept -> bool {
      if (state_.load(std::memory_order_acquire) == control_state::stopped) {
        return active_.load(std::memory_order_acquire) == 0U;
      }
      return remaining_.load(std::memory_order_acquire) == 0U;
    }

    auto finish_child(const std::uint32_t index, result_t status) noexcept
        -> void {
      const auto completed = emplace_result(index, std::move(status));
      wh_invariant(active_.load(std::memory_order_acquire) != 0U);
      active_.fetch_sub(1U, std::memory_order_acq_rel);
      if (!completed &&
          state_.load(std::memory_order_acquire) == control_state::started) {
        static_cast<void>(start_next());
      }
      arrive();
    }

    [[nodiscard]] auto start_next() noexcept -> start_result {
      if (state_.load(std::memory_order_acquire) != control_state::started) {
        return start_result::exhausted;
      }
      for (;;) {
        const auto index = next_.fetch_add(1U, std::memory_order_acq_rel);
        if (index >= senders_.size()) {
          return start_result::exhausted;
        }
        const auto started = start_child(index);
        if (started != start_result::advance) {
          return started;
        }
      }
    }

    [[nodiscard]] auto start_child(const std::size_t index) noexcept
        -> start_result {
      wh_invariant(index < senders_.size());
      wh_invariant(child_engaged_[index] == 0U);

      try {
        [[maybe_unused]] auto &child_op =
            child_ops_[index].construct_with([&]() -> child_op_t {
          return stdexec::connect(
              stdexec::starts_on(exec::trampoline_scheduler{},
                                 stdexec::write_env(
                                     std::move(senders_[index]),
                                     stdexec::prop{stdexec::get_stop_token,
                                                   stop_source_.get_token()})),
                                  child_receiver{
                                      this,
                                      static_cast<std::uint32_t>(index),
                                      env_,
                                  });
        });
        child_engaged_[index] = 1U;
        count_.fetch_add(1U, std::memory_order_relaxed);
        active_.fetch_add(1U, std::memory_order_relaxed);
        stdexec::start(child_ops_[index].get());
        return start_result::started;
      } catch (...) {
        if (child_engaged_[index] != 0U) {
          child_ops_[index].destruct();
          child_engaged_[index] = 0U;
        }
        if (emplace_result(index,
                           result_t::failure(wh::core::errc::internal_error))) {
          return start_result::completed;
        }
        return start_result::advance;
      }
    }

    [[nodiscard]] auto emplace_result(const std::size_t index,
                                      result_t status) noexcept -> bool {
      wh_invariant(index < result_engaged_.size());
      wh_invariant(result_engaged_[index] == 0U);
      [[maybe_unused]] auto &result = results_[index].construct(std::move(status));
      result_engaged_[index] = 1U;
      return remaining_.fetch_sub(1U, std::memory_order_acq_rel) == 1U;
    }

    auto complete() noexcept -> void {
      if (completed_.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      on_stop_.reset();

      const auto state = state_.load(std::memory_order_acquire);
      if (state == control_state::stopped) {
        stdexec::set_stopped(std::move(receiver_));
        return;
      }

      std::vector<result_t> results{};
      results.reserve(result_engaged_.size());
      for (std::size_t index = 0U; index < result_engaged_.size(); ++index) {
        if (result_engaged_[index] != 0U) {
          results.push_back(std::move(results_[index].get()));
          continue;
        }
        results.push_back(result_t::failure(wh::core::errc::internal_error));
      }

      stdexec::set_value(std::move(receiver_), std::move(results));
    }

    receiver_t receiver_;
    receiver_env_t env_;
    std::vector<sender_t> senders_{};
    std::unique_ptr<wh::core::detail::manual_lifetime<result_t>[]> results_{};
    std::vector<std::uint8_t> result_engaged_{};
    std::unique_ptr<wh::core::detail::manual_lifetime<child_op_t>[]>
        child_ops_{};
    std::vector<std::uint8_t> child_engaged_{};
    std::optional<stop_callback_t> on_stop_{};
    stdexec::inplace_stop_source stop_source_{};
    std::size_t max_in_flight_{0U};
    std::atomic<std::size_t> next_{0U};
    std::atomic<std::size_t> active_{0U};
    std::atomic<std::size_t> remaining_{0U};
    std::atomic<std::size_t> count_{1U};
    std::atomic<control_state> state_{control_state::started};
    std::atomic<bool> completed_{false};
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
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    return operation<stored_receiver_t>{std::move(self).senders_,
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
        std::vector<result_t>),
        stdexec::set_stopped_t()>{};
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
