#pragma once

#include <exception>
#include <concepts>
#include <functional>
#include <tuple>
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
  requires env_with_completion_scheduler<cpo_t, env_t>
struct resume_scheduler_selector<cpo_t, env_t, true> {
  using type = std::remove_cvref_t<
      decltype(stdexec::get_completion_scheduler<cpo_t>(
          std::declval<const env_t &>()))>;

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

template <typename env_t>
concept env_with_launch_scheduler =
    env_with_scheduler<env_t> ||
    env_with_completion_scheduler<stdexec::set_value_t, env_t>;

template <typename env_t, bool use_scheduler = env_with_scheduler<env_t>>
struct launch_scheduler_selector;

template <typename env_t>
  requires env_with_scheduler<env_t>
struct launch_scheduler_selector<env_t, true> {
  using type = std::remove_cvref_t<
      decltype(stdexec::get_scheduler(std::declval<const env_t &>()))>;

  [[nodiscard]] static constexpr auto
  get(const env_t &env) noexcept(noexcept(stdexec::get_scheduler(env))) -> type {
    return stdexec::get_scheduler(env);
  }
};

template <typename env_t>
  requires (!env_with_scheduler<env_t> &&
            env_with_completion_scheduler<stdexec::set_value_t, env_t>)
struct launch_scheduler_selector<env_t, false> {
  using type = std::remove_cvref_t<decltype(
      stdexec::get_completion_scheduler<stdexec::set_value_t>(
          std::declval<const env_t &>()))>;

  [[nodiscard]] static constexpr auto
  get(const env_t &env) noexcept(
      noexcept(stdexec::get_completion_scheduler<stdexec::set_value_t>(env)))
      -> type {
    return stdexec::get_completion_scheduler<stdexec::set_value_t>(env);
  }
};

template <typename env_t>
using selected_launch_scheduler_t =
    typename launch_scheduler_selector<std::remove_cvref_t<env_t>>::type;

template <typename env_t>
  requires env_with_launch_scheduler<env_t>
[[nodiscard]] constexpr auto
select_launch_scheduler(const env_t &env) noexcept(
    noexcept(launch_scheduler_selector<std::remove_cvref_t<env_t>>::get(env)))
    -> selected_launch_scheduler_t<env_t> {
  return launch_scheduler_selector<std::remove_cvref_t<env_t>>::get(env);
}

template <typename env_t>
using launch_scheduler_t = selected_launch_scheduler_t<env_t>;

struct get_launch_scheduler_t {
  template <typename env_t>
    requires env_with_launch_scheduler<env_t>
  [[nodiscard]] constexpr auto
  operator()(const env_t &env) const noexcept(
      noexcept(select_launch_scheduler(env))) -> launch_scheduler_t<env_t> {
    return select_launch_scheduler(env);
  }
};

inline constexpr get_launch_scheduler_t get_launch_scheduler{};

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
inline constexpr bool scheduler_query_v<stdexec::get_completion_domain_t<cpo_t>> =
    true;

template <typename outer_env_t, typename scheduler_t> struct scheduler_bound_env {
  /// Original receiver environment preserved for non-scheduler queries.
  [[no_unique_address]] outer_env_t outer_env{};
  /// Scheduler explicitly bound by the caller.
  [[no_unique_address]] scheduler_t scheduler{};

  /// Exposes the bound scheduler as the generic scheduler query.
  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
      -> scheduler_t {
    return scheduler;
  }

  /// Preserves all other environment queries from the wrapped receiver.
  template <typename query_t>
    requires(!scheduler_query_v<std::remove_cvref_t<query_t>> &&
             requires(const outer_env_t &outer, const query_t &query) {
               query(outer);
             })
  [[nodiscard]] auto query(const query_t &query) const
      noexcept(noexcept(query(outer_env))) -> decltype(query(outer_env)) {
    return query(outer_env);
  }
};

template <typename receiver_t, typename scheduler_t>
class scheduler_bound_receiver {
  using outer_env_t = std::remove_cvref_t<
      decltype(stdexec::get_env(std::declval<const receiver_t &>()))>;

public:
  using receiver_concept = stdexec::receiver_t;

  template <typename receiver_u, typename scheduler_u>
    requires std::constructible_from<receiver_t, receiver_u &&> &&
             std::constructible_from<scheduler_t, scheduler_u &&>
  explicit scheduler_bound_receiver(receiver_u &&receiver, scheduler_u &&scheduler)
      : receiver_(std::forward<receiver_u>(receiver)),
        scheduler_(std::forward<scheduler_u>(scheduler)) {}

  template <typename... value_t>
  auto set_value(value_t &&...value) && noexcept(
      noexcept(stdexec::set_value(std::move(receiver_),
                                  std::forward<value_t>(value)...))) -> void {
    stdexec::set_value(std::move(receiver_), std::forward<value_t>(value)...);
  }

  template <typename error_t>
  auto set_error(error_t &&error) && noexcept(
      noexcept(stdexec::set_error(std::move(receiver_),
                                  std::forward<error_t>(error)))) -> void {
    stdexec::set_error(std::move(receiver_), std::forward<error_t>(error));
  }

  auto set_stopped() && noexcept(
      noexcept(stdexec::set_stopped(std::move(receiver_)))) -> void {
    stdexec::set_stopped(std::move(receiver_));
  }

  [[nodiscard]] auto get_env() const noexcept
      -> scheduler_bound_env<outer_env_t, scheduler_t> {
    return scheduler_bound_env<outer_env_t, scheduler_t>{
        .outer_env = stdexec::get_env(receiver_),
        .scheduler = scheduler_,
    };
  }

private:
  receiver_t receiver_;
  [[no_unique_address]] scheduler_t scheduler_;
};

template <typename sender_t, typename scheduler_t>
class scheduler_bound_sender {
  template <typename receiver_t> class operation {
    using child_receiver_t =
        scheduler_bound_receiver<std::remove_cvref_t<receiver_t>, scheduler_t>;
    using child_op_t =
        stdexec::connect_result_t<std::remove_cvref_t<sender_t>, child_receiver_t>;

  public:
    template <typename sender_u, typename receiver_u, typename scheduler_u>
      requires std::constructible_from<child_receiver_t, receiver_u &&,
                                       scheduler_u &&> &&
               stdexec::sender_to<sender_u, child_receiver_t>
    explicit operation(sender_u &&sender, receiver_u &&receiver,
                       scheduler_u &&scheduler)
        : child_op_(stdexec::connect(
              std::forward<sender_u>(sender),
              child_receiver_t{std::forward<receiver_u>(receiver),
                               std::forward<scheduler_u>(scheduler)})) {}

    auto start() & noexcept -> void { stdexec::start(child_op_); }

  private:
    child_op_t child_op_;
  };

public:
  using sender_concept = stdexec::sender_t;

  template <typename sender_u, typename scheduler_u>
    requires std::constructible_from<sender_t, sender_u &&> &&
             std::constructible_from<scheduler_t, scheduler_u &&>
  explicit scheduler_bound_sender(sender_u &&sender, scheduler_u &&scheduler)
      : sender_(std::forward<sender_u>(sender)),
        scheduler_(std::forward<scheduler_u>(scheduler)) {}

  template <typename self_t, stdexec::receiver receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, scheduler_bound_sender> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>) &&
             stdexec::sender_to<std::remove_cvref_t<sender_t>,
                                scheduler_bound_receiver<
                                    std::remove_cvref_t<receiver_t>,
                                    std::remove_cvref_t<scheduler_t>>>
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                            receiver_t receiver)
      -> operation<receiver_t> {
    return operation<receiver_t>{std::move(self.sender_), std::move(receiver),
                                 std::move(self.scheduler_)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>, scheduler_bound_sender> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>) &&
             (sizeof...(env_t) >= 1U)
  static consteval auto get_completion_signatures() {
    using outer_env_t = std::tuple_element_t<0U, std::tuple<env_t...>>;
    using bound_env_t = scheduler_bound_env<std::remove_cvref_t<outer_env_t>,
                                            std::remove_cvref_t<scheduler_t>>;
    return stdexec::get_completion_signatures<std::remove_cvref_t<sender_t>,
                                              bound_env_t>();
  }

private:
  [[no_unique_address]] sender_t sender_;
  [[no_unique_address]] scheduler_t scheduler_;
};

template <stdexec::sender sender_t, stdexec::scheduler scheduler_t>
[[nodiscard]] constexpr auto bind_sender_scheduler(sender_t &&sender,
                                                   scheduler_t scheduler) {
  return scheduler_bound_sender<std::remove_cvref_t<sender_t>,
                                std::remove_cvref_t<scheduler_t>>{
      std::forward<sender_t>(sender), std::move(scheduler)};
}

using sender_signature_env =
    scheduler_bound_env<stdexec::env<>, any_resume_scheduler_t>;

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
