// Defines reusable receiver/env erasure helpers with scheduler-query policy.
#pragma once

#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::core::detail {

enum class missing_scheduler_mode : std::uint8_t {
  strict = 0U,
  fallback_inline,
};

template <missing_scheduler_mode Mode> struct erased_receiver_scheduler_policy;

template <> struct erased_receiver_scheduler_policy<missing_scheduler_mode::strict> {
  static constexpr bool requires_resume_scheduler = true;

  template <typename receiver_t>
  [[noreturn]] static auto missing_scheduler() noexcept -> any_resume_scheduler_t {
    static_assert(receiver_with_resume_scheduler<receiver_t>,
                  "async sender requires receiver env to expose scheduler or completion scheduler");
    std::abort();
  }
};

template <> struct erased_receiver_scheduler_policy<missing_scheduler_mode::fallback_inline> {
  static constexpr bool requires_resume_scheduler = false;

  template <typename receiver_t>
  [[nodiscard]] static auto missing_scheduler() noexcept -> any_resume_scheduler_t {
    return erase_resume_scheduler(stdexec::inline_scheduler{});
  }
};

template <typename policy_t> struct erased_receiver_env_vtable {
  any_resume_scheduler_t (*get_scheduler)(const void *) noexcept {nullptr};
  any_resume_scheduler_t (*get_delegation_scheduler)(const void *) noexcept {nullptr};
  any_resume_scheduler_t (*get_value_scheduler)(const void *) noexcept {nullptr};
  any_resume_scheduler_t (*get_error_scheduler)(const void *) noexcept {nullptr};
  any_resume_scheduler_t (*get_stopped_scheduler)(const void *) noexcept {nullptr};
  stdexec::inplace_stop_token (*get_stop_token)(const void *) noexcept {nullptr};
};

template <typename policy_t> class erased_receiver_env_ref {
public:
  erased_receiver_env_ref() = default;

  erased_receiver_env_ref(const void *object,
                          const erased_receiver_env_vtable<policy_t> *vtable) noexcept
      : object_(object), vtable_(vtable) {}

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept -> any_resume_scheduler_t {
    return vtable_->get_scheduler(object_);
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
      -> any_resume_scheduler_t {
    return vtable_->get_delegation_scheduler(object_);
  }

  [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
      -> any_resume_scheduler_t {
    return vtable_->get_value_scheduler(object_);
  }

  [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<stdexec::set_error_t>) const noexcept
      -> any_resume_scheduler_t {
    return vtable_->get_error_scheduler(object_);
  }

  [[nodiscard]] auto
  query(stdexec::get_completion_scheduler_t<stdexec::set_stopped_t>) const noexcept
      -> any_resume_scheduler_t {
    return vtable_->get_stopped_scheduler(object_);
  }

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
      -> stdexec::inplace_stop_token {
    return vtable_->get_stop_token(object_);
  }

private:
  const void *object_{nullptr};
  const erased_receiver_env_vtable<policy_t> *vtable_{nullptr};
};

template <typename policy_t, typename receiver_t> struct erased_receiver_env_model {
  template <typename cpo_t>
  [[nodiscard]] static auto select_completion_scheduler(const void *object) noexcept
      -> any_resume_scheduler_t {
    const auto &receiver = *static_cast<const receiver_t *>(object);
    const auto &env = stdexec::get_env(receiver);
    if constexpr (requires { wh::core::detail::select_resume_scheduler<cpo_t>(env); }) {
      return wh::core::detail::erase_resume_scheduler(
          wh::core::detail::select_resume_scheduler<cpo_t>(env));
    } else if constexpr (requires {
                           wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env);
                         }) {
      return wh::core::detail::erase_resume_scheduler(
          wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env));
    } else {
      return policy_t::template missing_scheduler<receiver_t>();
    }
  }

  [[nodiscard]] static auto get_scheduler(const void *object) noexcept -> any_resume_scheduler_t {
    const auto &receiver = *static_cast<const receiver_t *>(object);
    const auto &env = stdexec::get_env(receiver);
    if constexpr (requires { wh::core::detail::select_launch_scheduler(env); }) {
      return wh::core::detail::erase_resume_scheduler(
          wh::core::detail::select_launch_scheduler(env));
    } else if constexpr (requires {
                           wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env);
                         }) {
      return wh::core::detail::erase_resume_scheduler(
          wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env));
    } else {
      return policy_t::template missing_scheduler<receiver_t>();
    }
  }

  [[nodiscard]] static auto get_delegation_scheduler(const void *object) noexcept
      -> any_resume_scheduler_t {
    const auto &receiver = *static_cast<const receiver_t *>(object);
    const auto &env = stdexec::get_env(receiver);
    if constexpr (requires { stdexec::get_delegation_scheduler(env); }) {
      return wh::core::detail::erase_resume_scheduler(stdexec::get_delegation_scheduler(env));
    } else {
      return get_scheduler(object);
    }
  }

  [[nodiscard]] static auto get_stop_token(const void *object) noexcept
      -> stdexec::inplace_stop_token {
    const auto &receiver = *static_cast<const receiver_t *>(object);
    const auto &env = stdexec::get_env(receiver);
    if constexpr (requires {
                    { stdexec::get_stop_token(env) } -> std::same_as<stdexec::inplace_stop_token>;
                  }) {
      return stdexec::get_stop_token(env);
    } else {
      return {};
    }
  }

  static inline constexpr erased_receiver_env_vtable<policy_t> vtable{
      .get_scheduler = &erased_receiver_env_model::get_scheduler,
      .get_delegation_scheduler = &erased_receiver_env_model::get_delegation_scheduler,
      .get_value_scheduler =
          &erased_receiver_env_model::template select_completion_scheduler<stdexec::set_value_t>,
      .get_error_scheduler =
          &erased_receiver_env_model::template select_completion_scheduler<stdexec::set_error_t>,
      .get_stopped_scheduler =
          &erased_receiver_env_model::template select_completion_scheduler<stdexec::set_stopped_t>,
      .get_stop_token = &erased_receiver_env_model::get_stop_token,
  };
};

template <typename policy_t, typename... value_ts> struct erased_receiver_ref_vtable {
  void (*set_value)(void *, value_ts...) noexcept {nullptr};
  void (*set_error)(void *, std::exception_ptr) noexcept {nullptr};
  void (*set_stopped)(void *) noexcept {nullptr};
  erased_receiver_env_ref<policy_t> (*get_env)(const void *) noexcept {nullptr};
};

template <typename policy_t, typename receiver_t, typename... value_ts>
struct erased_receiver_ref_model {
  static auto set_value(void *object, value_ts... values) noexcept -> void {
    stdexec::set_value(std::move(*static_cast<receiver_t *>(object)), std::move(values)...);
  }

  static auto set_error(void *object, std::exception_ptr error) noexcept -> void {
    stdexec::set_error(std::move(*static_cast<receiver_t *>(object)), std::move(error));
  }

  static auto set_stopped(void *object) noexcept -> void {
    stdexec::set_stopped(std::move(*static_cast<receiver_t *>(object)));
  }

  [[nodiscard]] static auto get_env(const void *object) noexcept -> erased_receiver_env_ref<policy_t> {
    return erased_receiver_env_ref<policy_t>{object,
                                             &erased_receiver_env_model<policy_t, receiver_t>::vtable};
  }

  static inline constexpr erased_receiver_ref_vtable<policy_t, value_ts...> vtable{
      .set_value = &erased_receiver_ref_model::set_value,
      .set_error = &erased_receiver_ref_model::set_error,
      .set_stopped = &erased_receiver_ref_model::set_stopped,
      .get_env = &erased_receiver_ref_model::get_env,
  };
};

template <typename policy_t, typename... value_ts> class erased_receiver_ref {
public:
  using receiver_concept = stdexec::receiver_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(value_ts...),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;

  erased_receiver_ref() = default;
  erased_receiver_ref(const erased_receiver_ref &) noexcept = default;
  erased_receiver_ref(erased_receiver_ref &&) noexcept = default;
  auto operator=(const erased_receiver_ref &) noexcept -> erased_receiver_ref & = default;
  auto operator=(erased_receiver_ref &&) noexcept -> erased_receiver_ref & = default;

  template <typename receiver_t>
    requires(!std::same_as<std::remove_cvref_t<receiver_t>, erased_receiver_ref> &&
             stdexec::receiver_of<std::remove_cvref_t<receiver_t>, completion_signatures> &&
             (!policy_t::requires_resume_scheduler ||
              wh::core::detail::receiver_with_resume_scheduler<std::remove_cvref_t<receiver_t>>))
  erased_receiver_ref(receiver_t &receiver) noexcept
      : object_(std::addressof(receiver)),
        vtable_(
            &erased_receiver_ref_model<policy_t, std::remove_cvref_t<receiver_t>, value_ts...>::vtable) {}

  auto set_value(value_ts... values) noexcept -> void {
    vtable_->set_value(object_, std::move(values)...);
  }

  auto set_error(std::exception_ptr error) noexcept -> void {
    vtable_->set_error(object_, std::move(error));
  }

  auto set_stopped() noexcept -> void { vtable_->set_stopped(object_); }

  [[nodiscard]] auto get_env() const noexcept -> erased_receiver_env_ref<policy_t> {
    return vtable_->get_env(object_);
  }

private:
  void *object_{nullptr};
  const erased_receiver_ref_vtable<policy_t, value_ts...> *vtable_{nullptr};
};

} // namespace wh::core::detail
