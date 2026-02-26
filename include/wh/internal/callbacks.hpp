#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include "wh/core/concepts/callback_concepts.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/core/types/callback_types.hpp"

namespace wh::internal {

namespace detail {

[[nodiscard]] inline auto
default_timing_checker(const wh::core::callback_stage) noexcept -> bool {
  return true;
}

} // namespace detail

class callback_manager {
public:
  struct callback_registration {
    wh::core::callback_handler_config config{};
    wh::core::callback_stage_handler handler{};
  };

  using handler_list = wh::core::small_vector<callback_registration, 4U>;
  using shared_handler_list = std::shared_ptr<const handler_list>;

  callback_manager()
      : global_handlers_(std::make_shared<handler_list>()),
        local_handlers_(std::make_shared<handler_list>()) {}

  auto register_global_handler(wh::core::callback_handler_config config,
                               wh::core::callback_stage_handler handler)
      -> wh::core::result<void> {
    if (!static_cast<bool>(handler)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    ensure_default_timing_checker(config);

    callback_registration registration{};
    registration.config = std::move(config);
    registration.handler = std::move(handler);
    return append_handler(global_handlers_, std::move(registration));
  }

  auto register_local_handler(wh::core::callback_handler_config config,
                              wh::core::callback_stage_handler handler)
      -> wh::core::result<void> {
    if (!static_cast<bool>(handler)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    ensure_default_timing_checker(config);

    callback_registration registration{};
    registration.config = std::move(config);
    registration.handler = std::move(handler);
    return append_handler(local_handlers_, std::move(registration));
  }

  template <wh::core::CallbackHandlerLike handler_t>
    requires(!std::same_as<std::remove_cvref_t<handler_t>,
                           wh::core::callback_stage_handler>)
  auto register_global_handler(wh::core::callback_handler_config config,
                               handler_t &&handler)
      -> wh::core::result<void> {
    return register_global_handler(
        std::move(config),
        wh::core::callback_stage_handler{std::forward<handler_t>(handler)});
  }

  template <wh::core::CallbackHandlerLike handler_t>
    requires(!std::same_as<std::remove_cvref_t<handler_t>,
                           wh::core::callback_stage_handler>)
  auto register_local_handler(wh::core::callback_handler_config config,
                              handler_t &&handler)
      -> wh::core::result<void> {
    return register_local_handler(
        std::move(config),
        wh::core::callback_stage_handler{std::forward<handler_t>(handler)});
  }

  template <wh::core::TimingChecker timing_checker_t,
            wh::core::CallbackHandlerLike handler_t>
  auto register_global_handler(timing_checker_t &&timing_checker,
                               handler_t &&handler, std::string name = {})
      -> wh::core::result<void> {
    return register_global_handler(
        make_callback_config(std::forward<timing_checker_t>(timing_checker),
                             std::move(name)),
        std::forward<handler_t>(handler));
  }

  template <wh::core::TimingChecker timing_checker_t,
            wh::core::CallbackHandlerLike handler_t>
  auto register_local_handler(timing_checker_t &&timing_checker,
                              handler_t &&handler, std::string name = {})
      -> wh::core::result<void> {
    return register_local_handler(
        make_callback_config(std::forward<timing_checker_t>(timing_checker),
                             std::move(name)),
        std::forward<handler_t>(handler));
  }

  [[nodiscard]] auto dispatch(const wh::core::callback_stage stage,
                              const wh::core::callback_event_view event) const
      -> wh::core::result<void> {
    const auto global_handlers =
        std::atomic_load_explicit(&global_handlers_, std::memory_order_acquire);
    const auto local_handlers =
        std::atomic_load_explicit(&local_handlers_, std::memory_order_acquire);

    auto execute_handlers = [&](const handler_list &handlers,
                                const bool reverse_order)
        -> wh::core::result<void> {
      if (reverse_order) {
        for (auto iter = handlers.rbegin(); iter != handlers.rend(); ++iter) {
          if (!evaluate_timing_checker(*iter, stage)) {
            continue;
          }
          try {
            iter->handler(stage, event);
          } catch (...) {
            return wh::core::result<void>::failure(
                wh::core::errc::internal_error);
          }
        }
        return {};
      }

      for (const auto &entry : handlers) {
        if (!evaluate_timing_checker(entry, stage)) {
          continue;
        }
        try {
          entry.handler(stage, event);
        } catch (...) {
          return wh::core::result<void>::failure(wh::core::errc::internal_error);
        }
      }
      return {};
    };

    if (wh::core::is_reverse_callback_stage(stage)) {
      auto local_result = execute_handlers(*local_handlers, true);
      if (local_result.has_error()) {
        return local_result;
      }
      return execute_handlers(*global_handlers, true);
    }

    auto global_result = execute_handlers(*global_handlers, false);
    if (global_result.has_error()) {
      return global_result;
    }
    return execute_handlers(*local_handlers, false);
  }

  [[nodiscard]] auto global_handler_count() const noexcept -> std::size_t {
    const auto handlers =
        std::atomic_load_explicit(&global_handlers_, std::memory_order_acquire);
    return handlers->size();
  }

  [[nodiscard]] auto local_handler_count() const noexcept -> std::size_t {
    const auto handlers =
        std::atomic_load_explicit(&local_handlers_, std::memory_order_acquire);
    return handlers->size();
  }

private:
  auto append_handler(shared_handler_list &slot,
                      callback_registration registration)
      -> wh::core::result<void> {
    std::scoped_lock lock(mutex_);
    auto current_handlers =
        std::atomic_load_explicit(&slot, std::memory_order_acquire);
    auto next_handlers = std::make_shared<handler_list>(*current_handlers);
    const auto pushed = next_handlers->push_back(std::move(registration));
    if (pushed.has_error()) {
      return wh::core::result<void>::failure(pushed.error());
    }
    std::atomic_store_explicit(&slot, std::shared_ptr<const handler_list>{
                                          std::move(next_handlers)},
                               std::memory_order_release);
    return {};
  }

  static auto
  ensure_default_timing_checker(wh::core::callback_handler_config &config)
      -> void {
    if (static_cast<bool>(config.timing_checker)) {
      return;
    }
    config.timing_checker = detail::default_timing_checker;
  }

  [[nodiscard]] static auto evaluate_timing_checker(
      const callback_registration &registration,
      const wh::core::callback_stage stage) -> bool {
    if (!static_cast<bool>(registration.config.timing_checker)) {
      return true;
    }
    return registration.config.timing_checker(stage);
  }

  mutable std::mutex mutex_{};
  shared_handler_list global_handlers_{};
  shared_handler_list local_handlers_{};
};

template <wh::core::TimingChecker timing_checker_t>
[[nodiscard]] inline auto make_callback_config(timing_checker_t timing_checker,
                                               std::string name = {})
    -> wh::core::callback_handler_config {
  return wh::core::callback_handler_config{
      wh::core::callback_timing_checker{std::move(timing_checker)},
      std::move(name)};
}

template <typename... config_t>
[[nodiscard]] inline auto extract_callback_config(config_t... configs)
    -> wh::core::callback_handler_config {
  wh::core::callback_handler_config merged{};

  auto merge_one = [&](auto config) -> void {
    if (!config.name.empty()) {
      merged.name = std::move(config.name);
    }

    if (!static_cast<bool>(config.timing_checker)) {
      return;
    }

    if (!static_cast<bool>(merged.timing_checker)) {
      merged.timing_checker = std::move(config.timing_checker);
      return;
    }

    auto left_checker = std::move(merged.timing_checker);
    auto right_checker = std::move(config.timing_checker);
    merged.timing_checker =
        [left = std::move(left_checker),
         right = std::move(right_checker)](
            const wh::core::callback_stage stage) -> bool {
      return left(stage) && right(stage);
    };
  };

  (merge_one(std::move(configs)), ...);

  if (!static_cast<bool>(merged.timing_checker)) {
    merged.timing_checker = detail::default_timing_checker;
  }
  return merged;
}

template <typename payload_t>
auto inject_callback_event(wh::core::run_context &context,
                           const wh::core::callback_stage stage,
                           const payload_t &payload) -> wh::core::result<void> {
  if (!context.callback_manager) {
    return {};
  }

  return context.callback_manager->dispatch(stage,
                                            wh::core::make_callback_event_view(
                                                payload));
}

[[nodiscard]] inline auto capture_fatal_error() -> wh::core::callback_fatal_error {
  try {
    throw;
  } catch (const std::exception &error) {
    return wh::core::callback_fatal_error{
        wh::core::map_exception(error), error.what(),
        "stack-unavailable"};
  } catch (...) {
    return wh::core::callback_fatal_error{
        wh::core::make_error(wh::core::errc::internal_error), "unknown",
        "stack-unavailable"};
  }
}

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

template <typename callable_t>
auto run_with_fatal_event(wh::core::run_context &context, callable_t &&callable,
                          const wh::core::callback_stage fatal_stage =
                              wh::core::callback_stage::error)
    -> wh::core::result<void> {
  auto executed = run_with_fatal_error_capture(std::forward<callable_t>(callable));
  if (executed.has_value()) {
    return {};
  }

  const auto injected =
      inject_callback_event(context, fatal_stage, executed.error());
  if (injected.has_error()) {
    return injected;
  }
  return wh::core::result<void>::failure(executed.error().code);
}

} // namespace wh::internal
