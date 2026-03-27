#pragma once

#include <exception>
#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include <exec/any_sender_of.hpp>
#include <exec/completion_behavior.hpp>
#include <stdexec/execution.hpp>

namespace wh::core::detail {

template <typename cpo_t, typename env_t>
concept env_with_completion_scheduler =
    requires(const std::remove_cvref_t<env_t> &env) {
      stdexec::get_completion_scheduler<cpo_t>(env);
    };

template <typename env_t>
concept env_with_scheduler =
    requires(const std::remove_cvref_t<env_t> &env) {
      stdexec::get_scheduler(env);
    };

template <typename cpo_t, typename env_t>
concept env_with_resume_scheduler =
    env_with_completion_scheduler<cpo_t, env_t> || env_with_scheduler<env_t>;

template <typename cpo_t, typename env_t,
          bool use_completion_scheduler =
              env_with_completion_scheduler<cpo_t, env_t>>
struct resume_scheduler_selector;

template <typename cpo_t, typename env_t>
struct resume_scheduler_selector<cpo_t, env_t, true> {
  using type = std::remove_cvref_t<decltype(
      stdexec::get_completion_scheduler<cpo_t>(std::declval<const env_t &>()))>;

  [[nodiscard]] static constexpr auto
  get(const env_t &env) noexcept(
      noexcept(stdexec::get_completion_scheduler<cpo_t>(env))) -> type {
    return stdexec::get_completion_scheduler<cpo_t>(env);
  }
};

template <typename cpo_t, typename env_t>
  requires (!env_with_completion_scheduler<cpo_t, env_t> &&
            env_with_scheduler<env_t>)
struct resume_scheduler_selector<cpo_t, env_t, false> {
  using type = std::remove_cvref_t<
      decltype(stdexec::get_scheduler(std::declval<const env_t &>()))>;

  [[nodiscard]] static constexpr auto
  get(const env_t &env) noexcept(noexcept(stdexec::get_scheduler(env))) -> type {
    return stdexec::get_scheduler(env);
  }
};

template <typename cpo_t, typename env_t>
using selected_resume_scheduler_t =
    typename resume_scheduler_selector<cpo_t,
                                       std::remove_cvref_t<env_t>>::type;

template <typename cpo_t, typename env_t>
  requires env_with_resume_scheduler<cpo_t, env_t>
[[nodiscard]] constexpr auto
select_resume_scheduler(const env_t &env) noexcept(
    noexcept(resume_scheduler_selector<cpo_t,
                                       std::remove_cvref_t<env_t>>::get(env)))
    -> selected_resume_scheduler_t<cpo_t, env_t> {
  return resume_scheduler_selector<cpo_t,
                                   std::remove_cvref_t<env_t>>::get(env);
}

template <typename env_t>
using resume_scheduler_t =
    selected_resume_scheduler_t<stdexec::set_value_t, env_t>;

struct get_resume_scheduler_t {
  template <typename env_t>
    requires env_with_resume_scheduler<stdexec::set_value_t, env_t>
  [[nodiscard]] constexpr auto
  operator()(const env_t &env) const noexcept(
      noexcept(select_resume_scheduler<stdexec::set_value_t>(env)))
      -> resume_scheduler_t<env_t> {
    return select_resume_scheduler<stdexec::set_value_t>(env);
  }
};

inline constexpr get_resume_scheduler_t get_resume_scheduler{};

using any_resume_scheduler_t =
    exec::any_receiver_ref<stdexec::completion_signatures<
        stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>>::
        any_sender<>::any_scheduler<>;

template <stdexec::scheduler scheduler_t>
[[nodiscard]] inline auto erase_resume_scheduler(scheduler_t scheduler)
    -> any_resume_scheduler_t {
  return any_resume_scheduler_t{std::move(scheduler)};
}

[[nodiscard]] inline auto erase_resume_scheduler(any_resume_scheduler_t scheduler)
    -> any_resume_scheduler_t {
  return scheduler;
}

struct async_completion_env {
  [[nodiscard]] constexpr auto
  query(exec::get_completion_behavior_t<stdexec::set_value_t>) const noexcept {
    return exec::completion_behavior::asynchronous_affine;
  }

  [[nodiscard]] constexpr auto
  query(exec::get_completion_behavior_t<stdexec::set_error_t>) const noexcept {
    return exec::completion_behavior::asynchronous_affine;
  }

  [[nodiscard]] constexpr auto
  query(exec::get_completion_behavior_t<stdexec::set_stopped_t>) const noexcept {
    return exec::completion_behavior::asynchronous_affine;
  }

  template <typename cpo_t, typename env_t>
    requires requires(const env_t &env) {
      select_resume_scheduler<cpo_t>(env);
    }
  [[nodiscard]] constexpr auto
  query(stdexec::get_completion_scheduler_t<cpo_t>,
        const env_t &env) const noexcept
      -> decltype(select_resume_scheduler<cpo_t>(env)) {
    return select_resume_scheduler<cpo_t>(env);
  }

  template <typename cpo_t, typename env_t>
    requires requires(const env_t &env) {
      stdexec::get_completion_domain<cpo_t>(
          select_resume_scheduler<cpo_t>(env), env);
    }
  [[nodiscard]] constexpr auto
  query(stdexec::get_completion_domain_t<cpo_t>,
        const env_t &env) const noexcept
      -> decltype(
          stdexec::get_completion_domain<cpo_t>(
              select_resume_scheduler<cpo_t>(env), env)) {
    return stdexec::get_completion_domain<cpo_t>(
        select_resume_scheduler<cpo_t>(env), env);
  }
};

template <typename receiver_t>
concept receiver_with_resume_scheduler =
    env_with_resume_scheduler<stdexec::set_value_t,
                              stdexec::env_of_t<receiver_t>>;

template <typename promise_t>
concept promise_with_resume_scheduler =
    env_with_resume_scheduler<
        stdexec::set_value_t,
        decltype(stdexec::get_env(std::declval<promise_t &>()))>;

} // namespace wh::core::detail

namespace wh::core {

template <typename factory_t>
[[nodiscard]] constexpr auto read_resume_scheduler(factory_t &&factory) {
  return stdexec::read_env(detail::get_resume_scheduler) |
         stdexec::let_value(
             [factory = std::forward<factory_t>(factory)](
                 auto scheduler) mutable {
               return factory(std::move(scheduler));
             });
}

template <stdexec::sender sender_t, stdexec::scheduler scheduler_t>
[[nodiscard]] constexpr auto resume_on(sender_t &&sender, scheduler_t scheduler) {
  return stdexec::continues_on(std::forward<sender_t>(sender),
                               std::move(scheduler));
}

} // namespace wh::core
