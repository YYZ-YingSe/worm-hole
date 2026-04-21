#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/try_schedule.hpp"

namespace wh::core::detail::scheduler_handoff {

struct same_scheduler_t;

namespace detail {

template <typename scheduler_t>
concept same_scheduler_queryable =
    requires(const std::remove_cvref_t<scheduler_t> &scheduler, const same_scheduler_t &cpo) {
      { scheduler.query(cpo) } -> std::convertible_to<bool>;
    };

} // namespace detail

struct same_scheduler_t {
  template <typename scheduler_t>
  [[nodiscard]] constexpr auto operator()(const scheduler_t &scheduler) const noexcept -> bool {
    if constexpr (detail::same_scheduler_queryable<scheduler_t>) {
      return static_cast<bool>(scheduler.query(*this));
    } else {
      return false;
    }
  }
};

inline constexpr same_scheduler_t same_scheduler{};

template <typename scheduler_t> class schedule_handoff_sender;
template <typename scheduler_t> class try_schedule_handoff_sender;

template <typename sender_t>
concept schedule_handoff_sender_like = requires(const std::remove_cvref_t<sender_t> &sender) {
  typename std::remove_cvref_t<sender_t>::wh_scheduler_handoff_sender_tag;
  requires !std::remove_cvref_t<sender_t>::is_try_sender;
  sender.target_scheduler();
};

template <typename sender_t>
concept try_schedule_handoff_sender_like = requires(const std::remove_cvref_t<sender_t> &sender) {
  typename std::remove_cvref_t<sender_t>::wh_scheduler_handoff_sender_tag;
  requires std::remove_cvref_t<sender_t>::is_try_sender;
  sender.target_scheduler();
};

template <typename scheduler_t> struct sender_env {
  scheduler_t scheduler;

  template <typename cpo_t, typename... env_t>
  [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<cpo_t>,
                           const env_t &...) const noexcept -> scheduler_t {
    return scheduler;
  }

  template <typename cpo_t, typename... env_t>
    requires requires(const scheduler_t &inner_scheduler, const env_t &...env) {
      stdexec::get_completion_domain<cpo_t>(inner_scheduler, env...);
    }
  [[nodiscard]] auto query(stdexec::get_completion_domain_t<cpo_t>,
                           const env_t &...env) const noexcept
      -> decltype(stdexec::get_completion_domain<cpo_t>(scheduler, env...)) {
    return stdexec::get_completion_domain<cpo_t>(scheduler, env...);
  }
};

template <typename scheduler_t> class schedule_handoff_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::__completion_signatures_of_t<stdexec::schedule_result_t<scheduler_t>>;
  using wh_scheduler_handoff_sender_tag = void;
  static constexpr bool is_try_sender = false;

  explicit schedule_handoff_sender(scheduler_t scheduler) noexcept(
      std::is_nothrow_move_constructible_v<scheduler_t>)
      : scheduler_(std::move(scheduler)) {}

  template <typename receiver_t>
  [[nodiscard]] auto connect(receiver_t receiver) const
      -> stdexec::connect_result_t<stdexec::schedule_result_t<scheduler_t>, receiver_t> {
    return stdexec::connect(stdexec::schedule(scheduler_), std::move(receiver));
  }

  [[nodiscard]] auto get_env() const noexcept -> sender_env<scheduler_t> {
    return sender_env<scheduler_t>{scheduler_};
  }

  [[nodiscard]] auto target_scheduler() const noexcept -> const scheduler_t & { return scheduler_; }

private:
  scheduler_t scheduler_;
};

template <typename scheduler_t> class try_schedule_handoff_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::__completion_signatures_of_t<wh::core::try_schedule_result_t<scheduler_t>>;
  using wh_scheduler_handoff_sender_tag = void;
  static constexpr bool is_try_sender = true;

  explicit try_schedule_handoff_sender(scheduler_t scheduler) noexcept(
      std::is_nothrow_move_constructible_v<scheduler_t>)
      : scheduler_(std::move(scheduler)) {}

  template <typename receiver_t>
  [[nodiscard]] auto connect(receiver_t receiver) const
      -> stdexec::connect_result_t<wh::core::try_schedule_result_t<scheduler_t>, receiver_t> {
    return stdexec::connect(wh::core::try_schedule(scheduler_), std::move(receiver));
  }

  [[nodiscard]] auto get_env() const noexcept -> sender_env<scheduler_t> {
    return sender_env<scheduler_t>{scheduler_};
  }

  [[nodiscard]] auto target_scheduler() const noexcept -> const scheduler_t & { return scheduler_; }

private:
  scheduler_t scheduler_;
};

template <typename scheduler_t>
[[nodiscard]] auto make_schedule_handoff_sender(scheduler_t scheduler)
    -> schedule_handoff_sender<std::remove_cvref_t<scheduler_t>> {
  return schedule_handoff_sender<std::remove_cvref_t<scheduler_t>>{std::move(scheduler)};
}

template <typename scheduler_t>
  requires wh::core::try_scheduler<std::remove_cvref_t<scheduler_t>>
[[nodiscard]] auto make_try_schedule_handoff_sender(scheduler_t scheduler)
    -> try_schedule_handoff_sender<std::remove_cvref_t<scheduler_t>> {
  return try_schedule_handoff_sender<std::remove_cvref_t<scheduler_t>>{std::move(scheduler)};
}

} // namespace wh::core::detail::scheduler_handoff
