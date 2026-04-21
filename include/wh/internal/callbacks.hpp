// Defines internal callback dispatch helpers shared by component modules
// to invoke typed and payload callbacks consistently.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

#include "wh/core/callback.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::internal {

namespace detail {

/// Default timing checker that accepts all callback stages.
[[nodiscard]] inline auto default_timing_checker(const wh::core::callback_stage) noexcept -> bool {
  return true;
}

} // namespace detail

/// Callback registry and dispatcher.
class callback_manager {
public:
  /// One stage registration containing config and executable stage callback.
  struct stage_registration {
    /// Registration metadata such as stage filter and name.
    wh::core::callback_config config{};
    /// Stage-bound callback body.
    wh::core::stage_view_callback callback{nullptr};
  };

  /// Number of callback stages in `callback_stage`.
  static constexpr std::size_t stage_count =
      static_cast<std::size_t>(wh::core::callback_stage::stream_end) + 1U;
  /// Compact registration container for one stage bucket.
  using registration_list = wh::core::small_vector<stage_registration, 4U>;
  /// Immutable stage registration snapshot shared across dispatchers.
  using shared_registration_list = std::shared_ptr<const registration_list>;

  /// Copy-on-write publication slot for one stage snapshot.
  class registration_snapshot_slot {
  public:
    registration_snapshot_slot() = default;

    registration_snapshot_slot(const registration_snapshot_slot &other) noexcept {
      store(other.load(std::memory_order_acquire), std::memory_order_relaxed);
    }

    auto operator=(const registration_snapshot_slot &other) noexcept
        -> registration_snapshot_slot & {
      if (this == std::addressof(other)) {
        return *this;
      }
      store(other.load(std::memory_order_acquire), std::memory_order_release);
      return *this;
    }

    registration_snapshot_slot(registration_snapshot_slot &&other) noexcept {
      store(other.load(std::memory_order_acquire), std::memory_order_relaxed);
    }

    auto operator=(registration_snapshot_slot &&other) noexcept -> registration_snapshot_slot & {
      if (this == std::addressof(other)) {
        return *this;
      }
      store(other.load(std::memory_order_acquire), std::memory_order_release);
      return *this;
    }

    auto store(shared_registration_list snapshot, const std::memory_order order) -> void {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
      snapshot_.store(std::move(snapshot), order);
#else
      std::atomic_store_explicit(&snapshot_, std::move(snapshot), order);
#endif
    }

    [[nodiscard]] auto load(const std::memory_order order) const -> shared_registration_list {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
      return snapshot_.load(order);
#else
      return std::atomic_load_explicit(&snapshot_, order);
#endif
    }

    auto compare_exchange_weak(shared_registration_list &expected,
                               const shared_registration_list &desired,
                               const std::memory_order success, const std::memory_order failure)
        -> bool {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
      return snapshot_.compare_exchange_weak(expected, desired, success, failure);
#else
      return std::atomic_compare_exchange_weak_explicit(&snapshot_, &expected, desired, success,
                                                        failure);
#endif
    }

  private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<shared_registration_list> snapshot_{};
#else
    shared_registration_list snapshot_{};
#endif
  };

  /// Global stage buckets published with snapshot semantics.
  using global_registration_buckets = std::array<registration_snapshot_slot, stage_count>;
  /// Local stage buckets owned by current manager.
  using local_registration_buckets = std::array<registration_list, stage_count>;

  callback_manager() {
    for (auto &registrations : global_registrations_) {
      registrations.store(std::make_shared<registration_list>(), std::memory_order_relaxed);
    }
  }

  callback_manager(const callback_manager &) = default;
  callback_manager(callback_manager &&) noexcept = default;
  auto operator=(const callback_manager &) -> callback_manager & = default;
  auto operator=(callback_manager &&) noexcept -> callback_manager & = default;

  template <typename config_t, typename callbacks_t>
    requires wh::core::CallbackConfigLike<wh::core::remove_cvref_t<config_t>> &&
             std::same_as<wh::core::remove_cvref_t<callbacks_t>, wh::core::stage_callbacks>
  /// Registers one global stage-callback table by expanding to stage buckets.
  auto register_global_callbacks(config_t &&config, callbacks_t &&callbacks) -> void {
    wh::core::callback_config stable_config{std::forward<config_t>(config)};
    ensure_default_timing_checker(stable_config);
    const wh::core::stage_callbacks stable_callbacks{std::forward<callbacks_t>(callbacks)};

    for_each_stage([&](const wh::core::callback_stage stage) -> void {
      if (!stable_config.timing_checker(stage)) {
        return;
      }
      const auto *stage_callback = get_stage_callback(stable_callbacks, stage);
      if (stage_callback == nullptr || !static_cast<bool>(*stage_callback)) {
        return;
      }

      stage_registration registration{};
      registration.config = stable_config;
      registration.callback = make_stage_callback(*stage_callback);
      append_registration(global_registrations_[stage_index(stage)], std::move(registration));
    });
  }

  template <typename config_t, typename callbacks_t>
    requires wh::core::CallbackConfigLike<wh::core::remove_cvref_t<config_t>> &&
             std::same_as<wh::core::remove_cvref_t<callbacks_t>, wh::core::stage_callbacks>
  /// Registers one local stage-callback table by expanding to stage buckets.
  auto register_local_callbacks(config_t &&config, callbacks_t &&callbacks) -> void {
    wh::core::callback_config stable_config{std::forward<config_t>(config)};
    ensure_default_timing_checker(stable_config);
    const wh::core::stage_callbacks stable_callbacks{std::forward<callbacks_t>(callbacks)};

    for_each_stage([&](const wh::core::callback_stage stage) -> void {
      if (!stable_config.timing_checker(stage)) {
        return;
      }
      const auto *stage_callback = get_stage_callback(stable_callbacks, stage);
      if (stage_callback == nullptr || !static_cast<bool>(*stage_callback)) {
        return;
      }

      stage_registration registration{};
      registration.config = stable_config;
      registration.callback = make_stage_callback(*stage_callback);
      local_registrations_[stage_index(stage)].push_back(std::move(registration));
    });
  }

  template <wh::core::TimingChecker timing_checker_t, typename callbacks_t, typename name_t>
    requires std::same_as<wh::core::remove_cvref_t<callbacks_t>, wh::core::stage_callbacks> &&
             std::convertible_to<wh::core::remove_cvref_t<name_t>, std::string>
  /// Global registration overload that builds config from timing checker.
  auto register_global_callbacks(timing_checker_t &&timing_checker, callbacks_t &&callbacks,
                                 name_t &&name) -> void {
    return register_global_callbacks(
        make_callback_config(std::forward<timing_checker_t>(timing_checker),
                             std::string{std::forward<name_t>(name)}),
        std::forward<callbacks_t>(callbacks));
  }

  template <wh::core::TimingChecker timing_checker_t, typename callbacks_t, typename name_t>
    requires std::same_as<wh::core::remove_cvref_t<callbacks_t>, wh::core::stage_callbacks> &&
             std::convertible_to<wh::core::remove_cvref_t<name_t>, std::string>
  /// Local registration overload that builds config from timing checker.
  auto register_local_callbacks(timing_checker_t &&timing_checker, callbacks_t &&callbacks,
                                name_t &&name) -> void {
    return register_local_callbacks(
        make_callback_config(std::forward<timing_checker_t>(timing_checker),
                             std::string{std::forward<name_t>(name)}),
        std::forward<callbacks_t>(callbacks));
  }

  template <wh::core::TimingChecker timing_checker_t, typename callbacks_t>
    requires std::same_as<wh::core::remove_cvref_t<callbacks_t>, wh::core::stage_callbacks>
  auto register_global_callbacks(timing_checker_t &&timing_checker, callbacks_t &&callbacks)
      -> void {
    return register_global_callbacks(std::forward<timing_checker_t>(timing_checker),
                                     std::forward<callbacks_t>(callbacks), std::string{});
  }

  /// Local registration overload with default name.
  template <wh::core::TimingChecker timing_checker_t, typename callbacks_t>
    requires std::same_as<wh::core::remove_cvref_t<callbacks_t>, wh::core::stage_callbacks>
  auto register_local_callbacks(timing_checker_t &&timing_checker, callbacks_t &&callbacks)
      -> void {
    return register_local_callbacks(std::forward<timing_checker_t>(timing_checker),
                                    std::forward<callbacks_t>(callbacks), std::string{});
  }

  /// Dispatches one callback event through global/local registration pipelines.
  auto dispatch(const wh::core::callback_stage stage, const wh::core::callback_event_view event,
                const wh::core::callback_run_info &run_info) const -> void {
    const std::size_t current_stage_index = stage_index(stage);
    const auto global_registrations = load_snapshot(global_registrations_[current_stage_index]);

    auto execute_registrations = [&](const registration_list &registrations,
                                     const bool reverse_order) -> void {
      if (reverse_order) {
        for (auto iter = registrations.rbegin(); iter != registrations.rend(); ++iter) {
          iter->callback(stage, event, run_info);
        }
        return;
      }

      for (const auto &entry : registrations) {
        entry.callback(stage, event, run_info);
      }
    };

    if (wh::core::is_reverse_callback_stage(stage)) {
      execute_registrations(local_registrations_[current_stage_index], true);
      execute_registrations(*global_registrations, true);
      return;
    }

    execute_registrations(*global_registrations, false);
    execute_registrations(local_registrations_[current_stage_index], false);
  }

  /// Dispatches one owning payload to a single callback.
  auto dispatch_single(const wh::core::callback_stage stage,
                       wh::core::callback_event_payload &&payload,
                       const wh::core::callback_run_info &run_info,
                       const wh::core::stage_payload_callback &callback) const -> void {
    if (!static_cast<bool>(callback)) {
      return;
    }
    callback(stage, std::move(payload), run_info);
  }

  /// Rvalue callback overload that forwards to stable const-reference path.
  auto dispatch_single(const wh::core::callback_stage stage,
                       wh::core::callback_event_payload &&payload,
                       const wh::core::callback_run_info &run_info,
                       wh::core::stage_payload_callback &&callback) const -> void {
    return dispatch_single(stage, std::move(payload), run_info,
                           static_cast<const wh::core::stage_payload_callback &>(callback));
  }

  /// Single-dispatch overload for callback-like payload callbacks.
  template <wh::core::StagePayloadCallbackLike callback_t>
    requires(!std::same_as<wh::core::remove_cvref_t<callback_t>, wh::core::stage_payload_callback>)
  auto dispatch_single(const wh::core::callback_stage stage,
                       wh::core::callback_event_payload &&payload,
                       const wh::core::callback_run_info &run_info, callback_t &&callback) const
      -> void {
    return dispatch_single(stage, std::move(payload), run_info,
                           wh::core::stage_payload_callback{std::forward<callback_t>(callback)});
  }

  /// Single-dispatch overload that materializes owning payload from typed
  /// value.
  template <typename payload_t, wh::core::StagePayloadCallbackLike callback_t>
  auto dispatch_single(const wh::core::callback_stage stage, payload_t &&payload,
                       const wh::core::callback_run_info &run_info, callback_t &&callback) const
      -> void {
    return dispatch_single(stage,
                           wh::core::make_callback_event_payload(std::forward<payload_t>(payload)),
                           run_info, std::forward<callback_t>(callback));
  }

  /// Returns current global registration count.
  [[nodiscard]] auto global_registration_count() const noexcept -> std::size_t {
    std::size_t count = 0U;
    std::ranges::for_each(global_registrations_,
                          [&count](const registration_snapshot_slot &slot) -> void {
                            const auto registrations = load_snapshot(slot);
                            count += registrations->size();
                          });
    return count;
  }

  /// Returns current local registration count.
  [[nodiscard]] auto local_registration_count() const noexcept -> std::size_t {
    std::size_t count = 0U;
    std::ranges::for_each(local_registrations_,
                          [&count](const registration_list &registrations) -> void {
                            count += registrations.size();
                          });
    return count;
  }

private:
  template <typename callback_t> static auto for_each_stage(callback_t &&callback) -> void {
    for (std::size_t index = 0U; index < stage_count; ++index) {
      callback(static_cast<wh::core::callback_stage>(index));
    }
  }

  [[nodiscard]] static constexpr auto stage_index(const wh::core::callback_stage stage) noexcept
      -> std::size_t {
    return static_cast<std::size_t>(stage);
  }

  [[nodiscard]] static auto make_stage_callback(const wh::core::stage_view_callback &callback)
      -> wh::core::stage_view_callback {
    return callback;
  }

  [[nodiscard]] static auto load_snapshot(const registration_snapshot_slot &slot)
      -> shared_registration_list {
    return slot.load(std::memory_order_acquire);
  }

  [[nodiscard]] static auto get_stage_callback(const wh::core::stage_callbacks &callbacks,
                                               const wh::core::callback_stage stage)
      -> const wh::core::stage_view_callback * {
    switch (stage) {
    case wh::core::callback_stage::start:
      return &callbacks.on_start;
    case wh::core::callback_stage::end:
      return &callbacks.on_end;
    case wh::core::callback_stage::error:
      return &callbacks.on_error;
    case wh::core::callback_stage::stream_start:
      return &callbacks.on_stream_start;
    case wh::core::callback_stage::stream_end:
      return &callbacks.on_stream_end;
    }
    return nullptr;
  }

  /// Appends one registration by creating and publishing a new snapshot.
  auto append_registration(registration_snapshot_slot &slot, stage_registration &&registration)
      -> void {
    const stage_registration stable_registration{std::move(registration)};

    auto current_registrations = load_snapshot(slot);
    while (true) {
      auto next_registrations = std::make_shared<registration_list>(*current_registrations);
      auto registration_copy = stable_registration;
      next_registrations->push_back(std::move(registration_copy));

      shared_registration_list next_snapshot{std::move(next_registrations)};
      if (slot.compare_exchange_weak(current_registrations, next_snapshot,
                                     std::memory_order_release, std::memory_order_acquire)) {
        return;
      }
    }
  }

  /// Ensures config always has a valid stage checker.
  static auto ensure_default_timing_checker(wh::core::callback_config &config) -> void {
    if (static_cast<bool>(config.timing_checker)) {
      return;
    }
    config.timing_checker = detail::default_timing_checker;
  }

  /// Global stage buckets published via copy-on-write snapshots.
  global_registration_buckets global_registrations_{};
  /// Local stage buckets registered for the current manager scope.
  local_registration_buckets local_registrations_{};
};

/// Builds registration config from timing checker and optional debug name.
template <wh::core::TimingChecker timing_checker_t, typename name_t = std::string>
  requires std::constructible_from<std::string, name_t &&>
[[nodiscard]] inline auto make_callback_config(timing_checker_t &&timing_checker,
                                               name_t &&name = {}) -> wh::core::callback_config {
  return wh::core::callback_config{
      wh::core::callback_timing_checker{std::forward<timing_checker_t>(timing_checker)},
      std::string{std::forward<name_t>(name)}};
}

/// Merges multiple registration configs into one effective config.
template <typename... config_t>
[[nodiscard]] inline auto merge_callback_config(config_t &&...configs)
    -> wh::core::callback_config {
  wh::core::callback_config merged{};

  auto merge_one = [&](auto &&config) -> void {
    auto stable_config = wh::core::callback_config{std::forward<decltype(config)>(config)};
    if (!stable_config.name.empty()) {
      merged.name = std::move(stable_config.name);
    }

    if (!static_cast<bool>(stable_config.timing_checker)) {
      return;
    }

    if (!static_cast<bool>(merged.timing_checker)) {
      merged.timing_checker = std::move(stable_config.timing_checker);
      return;
    }

    auto left_checker = std::move(merged.timing_checker);
    auto right_checker = std::move(stable_config.timing_checker);
    merged.timing_checker = [left = std::move(left_checker), right = std::move(right_checker)](
                                const wh::core::callback_stage stage) -> bool {
      return left(stage) && right(stage);
    };
  };

  (merge_one(std::forward<config_t>(configs)), ...);

  if (!static_cast<bool>(merged.timing_checker)) {
    merged.timing_checker = detail::default_timing_checker;
  }
  return merged;
}

} // namespace wh::internal
