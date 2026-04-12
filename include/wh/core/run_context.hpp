// Defines per-run context storage for typed values, metadata, and callback
// state shared during component execution.
#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/resume_state.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/internal/callbacks.hpp"
#include "wh/internal/stacktrace.hpp"

namespace wh::core {
namespace detail {

/// Transparent hash alias for heterogeneous lookup in session store.
using session_string_hash = wh::core::transparent_string_hash;

/// Transparent equality alias for heterogeneous lookup.
using session_string_equal = wh::core::transparent_string_equal;

} // namespace detail

/// Callback manager plus invoke-scoped metadata shared during one run.
struct callback_runtime {
  wh::internal::callback_manager manager{};
  callback_run_metadata metadata{};
};

/// Per-run mutable state shared across components.
struct run_context {
  using session_store = std::unordered_map<std::string, wh::core::any,
                                           detail::session_string_hash,
                                           detail::session_string_equal>;

  session_store session_values{};
  std::optional<callback_runtime> callbacks{};
  std::optional<interrupt_context> interrupt_info{};
  std::optional<resume_state> resume_info{};
};

[[nodiscard]] auto clone_run_context(const run_context &context)
    -> result<run_context>;

/// Stores a typed session value under `key`.
template <typename key_t, typename value_t>
  requires std::constructible_from<std::string, key_t &&>
auto set_session_value(run_context &context, key_t &&key, value_t &&value)
    -> result<void> {
  std::string stored_key{std::forward<key_t>(key)};
  using stored_t = remove_cvref_t<value_t>;
  wh::core::any stored_value{};
  if constexpr (std::same_as<stored_t, wh::core::any>) {
    stored_value = std::forward<value_t>(value);
  } else {
    stored_value = wh::core::any{std::in_place_type<stored_t>,
                                 std::forward<value_t>(value)};
  }
  auto owned = wh::core::into_owned(std::move(stored_value));
  if (owned.has_error()) {
    return result<void>::failure(owned.error());
  }
  context.session_values.insert_or_assign(std::move(stored_key),
                                          std::move(owned).value());
  return {};
}

/// Gets an immutable typed reference from session storage.
template <typename value_t>
[[nodiscard]] auto session_value_ref(const run_context &context,
                                     const std::string_view key)
    -> result<std::reference_wrapper<const value_t>> {
  const auto iter = context.session_values.find(key);
  if (iter == context.session_values.end()) {
    return result<std::reference_wrapper<const value_t>>::failure(
        errc::not_found);
  }

  const auto *typed = wh::core::any_cast<value_t>(&iter->second);
  if (typed == nullptr) {
    return result<std::reference_wrapper<const value_t>>::failure(
        errc::type_mismatch);
  }
  return std::cref(*typed);
}

/// Gets a mutable typed reference from session storage.
template <typename value_t>
[[nodiscard]] auto session_value_ref(run_context &context,
                                     const std::string_view key)
    -> result<std::reference_wrapper<value_t>> {
  const auto iter = context.session_values.find(key);
  if (iter == context.session_values.end()) {
    return result<std::reference_wrapper<value_t>>::failure(errc::not_found);
  }

  auto *typed = wh::core::any_cast<value_t>(&iter->second);
  if (typed == nullptr) {
    return result<std::reference_wrapper<value_t>>::failure(
        errc::type_mismatch);
  }
  return std::ref(*typed);
}

/// Moves a typed value out of session storage and erases the key.
template <typename value_t>
[[nodiscard]] auto consume_session_value(run_context &context,
                                         const std::string_view key)
    -> result<value_t> {
  const auto iter = context.session_values.find(key);
  if (iter == context.session_values.end()) {
    return result<value_t>::failure(errc::not_found);
  }

  auto *typed = wh::core::any_cast<value_t>(&iter->second);
  if (typed == nullptr) {
    return result<value_t>::failure(errc::type_mismatch);
  }

  value_t moved = std::move(*typed);
  context.session_values.erase(iter);
  return moved;
}

/// Returns whether callback manager is attached.
[[nodiscard]] inline auto
has_callback_manager(const run_context &context) noexcept -> bool {
  return context.callbacks.has_value();
}

/// Registers one local stage-callback table on context callback manager and
/// returns context.
template <typename config_t, typename callbacks_t>
  requires wh::core::CallbackConfigLike<wh::core::remove_cvref_t<config_t>> &&
           std::same_as<wh::core::remove_cvref_t<callbacks_t>,
                        wh::core::stage_callbacks>
[[nodiscard]] inline auto
register_local_callbacks(wh::core::run_context &&context, config_t &&config,
                         callbacks_t &&callbacks)
    -> wh::core::result<wh::core::run_context> {
  if (!context.callbacks.has_value()) {
    return wh::core::result<wh::core::run_context>::failure(
        wh::core::errc::not_found);
  }

  context.callbacks->manager.register_local_callbacks(
      std::forward<config_t>(config), std::forward<callbacks_t>(callbacks));
  return context;
}

/// Registers one local stage-callback table on context callback manager and
/// returns context.
template <typename config_t, typename callbacks_t>
  requires wh::core::CallbackConfigLike<wh::core::remove_cvref_t<config_t>> &&
           std::same_as<wh::core::remove_cvref_t<callbacks_t>,
                        wh::core::stage_callbacks>
[[nodiscard]] inline auto
register_local_callbacks(const wh::core::run_context &context,
                         config_t &&config, callbacks_t &&callbacks)
    -> wh::core::result<wh::core::run_context> {
  auto copied = clone_run_context(context);
  if (copied.has_error()) {
    return wh::core::result<wh::core::run_context>::failure(copied.error());
  }
  return register_local_callbacks(std::move(copied).value(),
                                  std::forward<config_t>(config),
                                  std::forward<callbacks_t>(callbacks));
}

/// Timing-checker overload for local registration with context return.
template <wh::core::TimingChecker timing_checker_t, typename callbacks_t,
          typename name_t>
  requires std::same_as<wh::core::remove_cvref_t<callbacks_t>,
                        wh::core::stage_callbacks> &&
           std::convertible_to<wh::core::remove_cvref_t<name_t>, std::string>
[[nodiscard]] inline auto
register_local_callbacks(wh::core::run_context &&context,
                         timing_checker_t &&timing_checker,
                         callbacks_t &&callbacks, name_t &&name)
    -> wh::core::result<wh::core::run_context> {
  return register_local_callbacks(
      std::move(context),
      wh::internal::make_callback_config(
          std::forward<timing_checker_t>(timing_checker),
          std::string{std::forward<name_t>(name)}),
      std::forward<callbacks_t>(callbacks));
}

/// Timing-checker overload for local registration with context return.
template <wh::core::TimingChecker timing_checker_t, typename callbacks_t,
          typename name_t>
  requires std::same_as<wh::core::remove_cvref_t<callbacks_t>,
                        wh::core::stage_callbacks> &&
           std::convertible_to<wh::core::remove_cvref_t<name_t>, std::string>
[[nodiscard]] inline auto
register_local_callbacks(const wh::core::run_context &context,
                         timing_checker_t &&timing_checker,
                         callbacks_t &&callbacks, name_t &&name)
    -> wh::core::result<wh::core::run_context> {
  return register_local_callbacks(
      context,
      wh::internal::make_callback_config(
          std::forward<timing_checker_t>(timing_checker),
          std::string{std::forward<name_t>(name)}),
      std::forward<callbacks_t>(callbacks));
}

/// Emits one callback event through context callback manager if present.
template <typename payload_t>
auto inject_callback_event(wh::core::run_context &context,
                           const wh::core::callback_stage stage,
                           const payload_t &payload,
                           const wh::core::callback_run_info &run_info)
    -> void {
  if (!context.callbacks.has_value()) {
    return;
  }

  if (!context.callbacks->metadata.empty()) {
    context.callbacks->manager.dispatch(
        stage, wh::core::make_callback_event_view(payload),
        wh::core::apply_callback_run_metadata(run_info,
                                              context.callbacks->metadata));
    return;
  }

  context.callbacks->manager.dispatch(
      stage, wh::core::make_callback_event_view(payload), run_info);
}

/// Captures current exception into structured fatal callback payload.
[[nodiscard]] inline auto capture_fatal_error()
    -> wh::core::callback_fatal_error {
  try {
    throw;
  } catch (const std::exception &error) {
    return wh::core::callback_fatal_error{wh::core::map_exception(error),
                                          error.what(),
                                          wh::internal::capture_call_stack()};
  } catch (...) {
    return wh::core::callback_fatal_error{
        wh::core::make_error(wh::core::errc::internal_error), "unknown",
        wh::internal::capture_call_stack()};
  }
}

/// Executes callable and converts thrown exception into fatal-error result.
template <typename callable_t>
[[nodiscard]] auto run_with_fatal_error_capture(callable_t &&callable)
    -> wh::core::result<void, wh::core::callback_fatal_error> {
  try {
    std::forward<callable_t>(callable)();
    return {};
  } catch (...) {
    return wh::core::result<void, wh::core::callback_fatal_error>::failure(
        capture_fatal_error());
  }
}

/// Executes callable and emits fatal callback event when it throws.
template <typename callable_t>
auto run_with_fatal_event(wh::core::run_context &context, callable_t &&callable,
                          const wh::core::callback_stage fatal_stage =
                              wh::core::callback_stage::error) -> void {
  auto executed =
      run_with_fatal_error_capture(std::forward<callable_t>(callable));
  if (executed.has_value()) {
    return;
  }

  inject_callback_event(context, fatal_stage, executed.error(),
                        wh::core::callback_run_info{});
}

template <> struct any_owned_traits<run_context> {
  [[nodiscard]] static auto into_owned(const run_context &value)
      -> result<run_context> {
    auto session_values = wh::core::into_owned_any_map(value.session_values);
    if (session_values.has_error()) {
      return result<run_context>::failure(session_values.error());
    }

    std::optional<interrupt_context> interrupt_info{};
    if (value.interrupt_info.has_value()) {
      auto owned_interrupt = wh::core::into_owned(*value.interrupt_info);
      if (owned_interrupt.has_error()) {
        return result<run_context>::failure(owned_interrupt.error());
      }
      interrupt_info = std::move(owned_interrupt).value();
    }

    std::optional<resume_state> resume_info{};
    if (value.resume_info.has_value()) {
      auto owned_resume = wh::core::into_owned(*value.resume_info);
      if (owned_resume.has_error()) {
        return result<run_context>::failure(owned_resume.error());
      }
      resume_info = std::move(owned_resume).value();
    }

    return run_context{
        .session_values = std::move(session_values).value(),
        .callbacks = value.callbacks,
        .interrupt_info = std::move(interrupt_info),
        .resume_info = std::move(resume_info),
    };
  }

  [[nodiscard]] static auto into_owned(run_context &&value)
      -> result<run_context> {
    auto session_values =
        wh::core::into_owned_any_map(std::move(value.session_values));
    if (session_values.has_error()) {
      return result<run_context>::failure(session_values.error());
    }

    std::optional<interrupt_context> interrupt_info{};
    if (value.interrupt_info.has_value()) {
      auto owned_interrupt = wh::core::into_owned(std::move(*value.interrupt_info));
      if (owned_interrupt.has_error()) {
        return result<run_context>::failure(owned_interrupt.error());
      }
      interrupt_info = std::move(owned_interrupt).value();
    }

    std::optional<resume_state> resume_info{};
    if (value.resume_info.has_value()) {
      auto owned_resume = wh::core::into_owned(std::move(*value.resume_info));
      if (owned_resume.has_error()) {
        return result<run_context>::failure(owned_resume.error());
      }
      resume_info = std::move(owned_resume).value();
    }

    return run_context{
        .session_values = std::move(session_values).value(),
        .callbacks = std::move(value.callbacks),
        .interrupt_info = std::move(interrupt_info),
        .resume_info = std::move(resume_info),
    };
  }
};

[[nodiscard]] inline auto clone_run_context(const run_context &context)
    -> result<run_context> {
  return wh::core::into_owned(context);
}

} // namespace wh::core
