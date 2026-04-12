#pragma once

#include <concepts>
#include <exception>
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
concept env_with_scheduler = requires(const std::remove_cvref_t<env_t> &env) {
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
  requires env_with_completion_scheduler<cpo_t, env_t>
struct resume_scheduler_selector<cpo_t, env_t, true> {
  using type =
      std::remove_cvref_t<decltype(stdexec::get_completion_scheduler<cpo_t>(
          std::declval<const env_t &>()))>;

  [[nodiscard]] static constexpr auto get(const env_t &env) noexcept(
      noexcept(stdexec::get_completion_scheduler<cpo_t>(env))) -> type {
    return stdexec::get_completion_scheduler<cpo_t>(env);
  }
};

template <typename cpo_t, typename env_t>
  requires(!env_with_completion_scheduler<cpo_t, env_t> &&
           env_with_scheduler<env_t>)
struct resume_scheduler_selector<cpo_t, env_t, false> {
  using type = std::remove_cvref_t<decltype(stdexec::get_scheduler(
      std::declval<const env_t &>()))>;

  [[nodiscard]] static constexpr auto
  get(const env_t &env) noexcept(noexcept(stdexec::get_scheduler(env)))
      -> type {
    return stdexec::get_scheduler(env);
  }
};

template <typename cpo_t, typename env_t>
using selected_resume_scheduler_t =
    typename resume_scheduler_selector<cpo_t, std::remove_cvref_t<env_t>>::type;

template <typename cpo_t, typename env_t>
  requires env_with_resume_scheduler<cpo_t, env_t>
[[nodiscard]] constexpr auto
select_resume_scheduler(const env_t &env) noexcept(noexcept(
    resume_scheduler_selector<cpo_t, std::remove_cvref_t<env_t>>::get(env)))
    -> selected_resume_scheduler_t<cpo_t, env_t> {
  return resume_scheduler_selector<cpo_t, std::remove_cvref_t<env_t>>::get(env);
}

template <typename env_t>
using resume_scheduler_t =
    selected_resume_scheduler_t<stdexec::set_value_t, env_t>;

struct get_resume_scheduler_t {
  template <typename env_t>
    requires env_with_resume_scheduler<stdexec::set_value_t, env_t>
  [[nodiscard]] constexpr auto operator()(const env_t &env) const
      noexcept(noexcept(select_resume_scheduler<stdexec::set_value_t>(env)))
          -> resume_scheduler_t<env_t> {
    return select_resume_scheduler<stdexec::set_value_t>(env);
  }
};

inline constexpr get_resume_scheduler_t get_resume_scheduler{};

template <typename env_t>
concept env_with_launch_scheduler =
    env_with_scheduler<env_t> ||
    env_with_completion_scheduler<stdexec::set_value_t, env_t>;

template <typename env_t, bool use_scheduler = env_with_scheduler<env_t>>
struct launch_scheduler_selector;

template <typename env_t>
  requires env_with_scheduler<env_t>
struct launch_scheduler_selector<env_t, true> {
  using type = std::remove_cvref_t<decltype(stdexec::get_scheduler(
      std::declval<const env_t &>()))>;

  [[nodiscard]] static constexpr auto
  get(const env_t &env) noexcept(noexcept(stdexec::get_scheduler(env)))
      -> type {
    return stdexec::get_scheduler(env);
  }
};

template <typename env_t>
  requires(!env_with_scheduler<env_t> &&
           env_with_completion_scheduler<stdexec::set_value_t, env_t>)
struct launch_scheduler_selector<env_t, false> {
  using type = std::remove_cvref_t<
      decltype(stdexec::get_completion_scheduler<stdexec::set_value_t>(
          std::declval<const env_t &>()))>;

  [[nodiscard]] static constexpr auto get(const env_t &env) noexcept(noexcept(
      stdexec::get_completion_scheduler<stdexec::set_value_t>(env))) -> type {
    return stdexec::get_completion_scheduler<stdexec::set_value_t>(env);
  }
};

template <typename env_t>
using selected_launch_scheduler_t =
    typename launch_scheduler_selector<std::remove_cvref_t<env_t>>::type;

template <typename env_t>
  requires env_with_launch_scheduler<env_t>
[[nodiscard]] constexpr auto select_launch_scheduler(const env_t &env) noexcept(
    noexcept(launch_scheduler_selector<std::remove_cvref_t<env_t>>::get(env)))
    -> selected_launch_scheduler_t<env_t> {
  return launch_scheduler_selector<std::remove_cvref_t<env_t>>::get(env);
}

template <typename env_t>
using launch_scheduler_t = selected_launch_scheduler_t<env_t>;

struct get_launch_scheduler_t {
  template <typename env_t>
    requires env_with_launch_scheduler<env_t>
  [[nodiscard]] constexpr auto operator()(const env_t &env) const
      noexcept(noexcept(select_launch_scheduler(env)))
          -> launch_scheduler_t<env_t> {
    return select_launch_scheduler(env);
  }
};

inline constexpr get_launch_scheduler_t get_launch_scheduler{};

using any_resume_scheduler_t =
    exec::any_receiver_ref<stdexec::completion_signatures<
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>>::any_sender<>::any_scheduler<>;

template <stdexec::scheduler scheduler_t>
[[nodiscard]] inline auto erase_resume_scheduler(scheduler_t scheduler)
    -> any_resume_scheduler_t {
  return any_resume_scheduler_t{std::move(scheduler)};
}

[[nodiscard]] inline auto
erase_resume_scheduler(any_resume_scheduler_t scheduler)
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

  [[nodiscard]] constexpr auto query(
      exec::get_completion_behavior_t<stdexec::set_stopped_t>) const noexcept {
    return exec::completion_behavior::asynchronous_affine;
  }

  template <typename cpo_t, typename env_t>
    requires requires(const env_t &env) { select_resume_scheduler<cpo_t>(env); }
  [[nodiscard]] constexpr auto query(stdexec::get_completion_scheduler_t<cpo_t>,
                                     const env_t &env) const noexcept
      -> decltype(select_resume_scheduler<cpo_t>(env)) {
    return select_resume_scheduler<cpo_t>(env);
  }

  template <typename cpo_t, typename env_t>
    requires requires(const env_t &env) {
      stdexec::get_completion_domain<cpo_t>(select_resume_scheduler<cpo_t>(env),
                                            env);
    }
  [[nodiscard]] constexpr auto query(stdexec::get_completion_domain_t<cpo_t>,
                                     const env_t &env) const noexcept
      -> decltype(stdexec::get_completion_domain<cpo_t>(
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
    env_with_resume_scheduler<stdexec::set_value_t,
                              decltype(stdexec::get_env(
                                  std::declval<promise_t &>()))>;

template <typename receiver_t>
concept receiver_with_launch_scheduler =
    env_with_launch_scheduler<stdexec::env_of_t<receiver_t>>;

template <typename query_t> inline constexpr bool scheduler_query_v = false;

template <>
inline constexpr bool scheduler_query_v<stdexec::get_scheduler_t> = true;

template <typename cpo_t>
inline constexpr bool
    scheduler_query_v<stdexec::get_completion_scheduler_t<cpo_t>> = true;

template <typename cpo_t>
inline constexpr bool
    scheduler_query_v<stdexec::get_completion_domain_t<cpo_t>> = true;

template <stdexec::scheduler scheduler_t> struct scheduler_query_env {
  /// Borrowed scheduler used to answer scheduler-related env queries.
  const scheduler_t *scheduler{nullptr};

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
      -> const scheduler_t & {
    return *scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
      -> const scheduler_t & {
    return *scheduler;
  }
};

template <typename outer_env_t, stdexec::scheduler scheduler_t>
struct scheduler_env {
  /// Borrowed outer environment preserved for non-scheduler queries.
  const outer_env_t *outer_env{nullptr};
  /// Borrowed scheduler fixed by the caller.
  const scheduler_t *scheduler{nullptr};

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
      -> const scheduler_t & {
    return *scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
      -> const scheduler_t & {
    return *scheduler;
  }

  template <typename query_t>
    requires(!scheduler_query_v<std::remove_cvref_t<query_t>> &&
             requires(const outer_env_t &env, const query_t &query) {
               query(env);
             })
  [[nodiscard]] auto query(const query_t &query) const
      noexcept(noexcept(query(*outer_env))) -> decltype(query(*outer_env)) {
    return query(*outer_env);
  }
};

template <stdexec::scheduler scheduler_t>
[[nodiscard]] constexpr auto
make_scheduler_queries(const scheduler_t &scheduler) noexcept
    -> scheduler_query_env<scheduler_t> {
  return scheduler_query_env<scheduler_t>{.scheduler = &scheduler};
}

template <stdexec::scheduler scheduler_t>
auto make_scheduler_queries(scheduler_t &&) = delete;

template <typename outer_env_t, stdexec::scheduler scheduler_t>
[[nodiscard]] constexpr auto
make_scheduler_env(const outer_env_t &outer_env,
                   const scheduler_t &scheduler) noexcept
    -> scheduler_env<std::remove_cvref_t<outer_env_t>, scheduler_t> {
  return scheduler_env<std::remove_cvref_t<outer_env_t>, scheduler_t>{
      .outer_env = &outer_env,
      .scheduler = &scheduler,
  };
}

template <typename outer_env_t, stdexec::scheduler scheduler_t>
auto make_scheduler_env(outer_env_t &&, scheduler_t &&) = delete;

template <stdexec::sender sender_t, stdexec::scheduler scheduler_t>
[[nodiscard]] constexpr auto
write_sender_scheduler(sender_t &&sender, const scheduler_t &scheduler) {
  return stdexec::write_env(std::forward<sender_t>(sender),
                            make_scheduler_queries(scheduler));
}

template <stdexec::sender sender_t, stdexec::scheduler scheduler_t>
auto write_sender_scheduler(sender_t &&, scheduler_t &&) = delete;

struct sender_signature_env {
  /// Compile-time-only scheduler placeholder used for signature probing.
  stdexec::inline_scheduler scheduler{};

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
      -> const stdexec::inline_scheduler & {
    return scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
      -> const stdexec::inline_scheduler & {
    return scheduler;
  }
};

} // namespace wh::core::detail

namespace wh::core {

template <typename factory_t>
[[nodiscard]] constexpr auto read_resume_scheduler(factory_t &&factory) {
  return stdexec::read_env(detail::get_resume_scheduler) |
         stdexec::let_value([factory = std::forward<factory_t>(factory)](
                                auto scheduler) mutable {
           return factory(std::move(scheduler));
         });
}

template <stdexec::sender sender_t, stdexec::scheduler scheduler_t>
[[nodiscard]] constexpr auto resume_on(sender_t &&sender,
                                       scheduler_t scheduler) {
  return stdexec::continues_on(std::forward<sender_t>(sender),
                               std::move(scheduler));
}

} // namespace wh::core
