// Defines the shared type-erased reducer registry core used by internal
// merge/concat registries.
#pragma once

#include <cstddef>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"

namespace wh::internal {

/// Type-erased value used by reducer registries.
using dynamic_reduce_value = wh::core::any;
/// Span view over type-erased reducer inputs.
using dynamic_reduce_values = std::span<const dynamic_reduce_value>;
/// Runtime reducer function signature for type-erased inputs.
using dynamic_reduce_function = wh::core::function<
    wh::core::result<dynamic_reduce_value>(dynamic_reduce_values) const>;

/// Shared storage and registration logic for typed/type-erased reducers.
class reduce_registry_core {
public:
  template <typename value_t>
  using typed_reduce_fn_t = wh::core::function<wh::core::result<value_t>(
      std::span<const value_t>) const>;

  template <typename value_t>
  using typed_ptr_reduce_fn_t = wh::core::function<wh::core::result<value_t>(
      std::span<const value_t *>) const>;

  reduce_registry_core() = default;

  /// Reserves dynamic and typed registration tables.
  auto reserve(const std::size_t type_count) -> void {
    dynamic_table_.reserve(type_count);
    typed_table_.reserve(type_count);
  }

  /// Freezes registry and rejects future registrations.
  auto freeze() noexcept -> void { frozen_ = true; }

  /// Returns whether registry is frozen.
  [[nodiscard]] auto is_frozen() const noexcept -> bool { return frozen_; }

  /// Registers one typed reducer and a dynamic bridge for `value_t`.
  template <typename value_t, typename function_t>
  auto register_reducer(function_t &&function_value) -> wh::core::result<void> {
    auto function =
        typed_reduce_fn_t<value_t>{std::forward<function_t>(function_value)};
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (!static_cast<bool>(function)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    const auto type = wh::core::any_type_key_v<value_t>;
    if (dynamic_table_.contains(type)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    auto bridge = typed_reduce_fn_t<value_t>{function};
    auto [typed_iter, typed_inserted] =
        typed_table_.emplace(type, std::move(function));
    if (!typed_inserted) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    const auto dynamic_inserted = dynamic_table_
                                      .emplace(
                                          type,
                                          [bridge = std::move(bridge)](
                                              const dynamic_reduce_values values)
                                              -> wh::core::result<dynamic_reduce_value> {
                                            wh::core::small_vector<value_t, 8U>
                                                typed_values{};
                                            typed_values.reserve(
                                                static_cast<typename decltype(
                                                    typed_values)::size_type>(
                                                    values.size()));
                                            for (const auto &value : values) {
                                              const auto *typed =
                                                  wh::core::any_cast<value_t>(&value);
                                              if (typed == nullptr) {
                                                return wh::core::result<
                                                    dynamic_reduce_value>::failure(
                                                    wh::core::errc::type_mismatch);
                                              }
                                              typed_values.push_back(*typed);
                                            }

                                            auto reduced = bridge(
                                                std::span<const value_t>{
                                                    typed_values.data(),
                                                    typed_values.size()});
                                            if (reduced.has_error()) {
                                              return wh::core::result<
                                                  dynamic_reduce_value>::failure(
                                                  reduced.error());
                                            }
                                            return dynamic_reduce_value{
                                                std::move(reduced).value()};
                                          })
                                      .second;
    if (!dynamic_inserted) {
      typed_table_.erase(typed_iter);
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    return {};
  }

  /// Registers one pointer-based typed reducer and a dynamic bridge.
  template <typename value_t, typename function_t>
  auto register_reducer_from_ptrs(function_t &&function_value)
      -> wh::core::result<void> {
    auto function = typed_ptr_reduce_fn_t<value_t>{
        std::forward<function_t>(function_value)};
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (!static_cast<bool>(function)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    auto typed_function = [function](const std::span<const value_t> values)
        -> wh::core::result<value_t> {
      wh::core::small_vector<const value_t *, 8U> typed_values{};
      typed_values.reserve(
          static_cast<typename decltype(typed_values)::size_type>(
              values.size()));
      for (const auto &value : values) {
        typed_values.push_back(std::addressof(value));
      }
      return function(
          std::span<const value_t *>{typed_values.data(), typed_values.size()});
    };

    const auto type = wh::core::any_type_key_v<value_t>;
    if (dynamic_table_.contains(type)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    auto [typed_iter, typed_inserted] =
        typed_table_.emplace(type, typed_reduce_fn_t<value_t>{typed_function});
    if (!typed_inserted) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    const auto dynamic_inserted = dynamic_table_
                                      .emplace(
                                          type,
                                          [function = std::move(function)](
                                              const dynamic_reduce_values values)
                                              -> wh::core::result<dynamic_reduce_value> {
                                            wh::core::small_vector<
                                                const value_t *, 8U>
                                                typed_values{};
                                            typed_values.reserve(
                                                static_cast<typename decltype(
                                                    typed_values)::size_type>(
                                                    values.size()));
                                            for (const auto &value : values) {
                                              const auto *typed =
                                                  wh::core::any_cast<value_t>(&value);
                                              if (typed == nullptr) {
                                                return wh::core::result<
                                                    dynamic_reduce_value>::failure(
                                                    wh::core::errc::type_mismatch);
                                              }
                                              typed_values.push_back(typed);
                                            }

                                            auto reduced = function(
                                                std::span<const value_t *>{
                                                    typed_values.data(),
                                                    typed_values.size()});
                                            if (reduced.has_error()) {
                                              return wh::core::result<
                                                  dynamic_reduce_value>::failure(
                                                  reduced.error());
                                            }
                                            return dynamic_reduce_value{
                                                std::move(reduced).value()};
                                          })
                                      .second;
    if (!dynamic_inserted) {
      typed_table_.erase(typed_iter);
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    return {};
  }

  /// Looks up one dynamic reducer by runtime type key.
  [[nodiscard]] auto find_dynamic(const wh::core::any_type_key type) const noexcept
      -> const dynamic_reduce_function * {
    const auto iter = dynamic_table_.find(type);
    if (iter == dynamic_table_.end()) {
      return nullptr;
    }
    return &iter->second;
  }

  /// Runs one dynamic reducer by runtime type key.
  [[nodiscard]] auto reduce(const wh::core::any_type_key type,
                            const dynamic_reduce_values values) const
      -> wh::core::result<dynamic_reduce_value> {
    if (values.empty()) {
      return wh::core::result<dynamic_reduce_value>::failure(
          wh::core::errc::invalid_argument);
    }

    if (const auto *function = find_dynamic(type); function != nullptr) {
      return (*function)(values);
    }

    return wh::core::result<dynamic_reduce_value>::failure(
        wh::core::errc::not_supported);
  }

  /// Looks up one typed reducer for `value_t`.
  template <typename value_t>
  [[nodiscard]] auto find_typed() const noexcept
      -> const typed_reduce_fn_t<value_t> * {
    const auto iter = typed_table_.find(wh::core::any_type_key_v<value_t>);
    if (iter == typed_table_.end()) {
      return nullptr;
    }

    return wh::core::any_cast<const typed_reduce_fn_t<value_t>>(&iter->second);
  }

  /// Number of registered reducer handlers.
  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return dynamic_table_.size();
  }

private:
  std::unordered_map<wh::core::any_type_key, dynamic_reduce_function,
                     wh::core::any_type_key_hash>
      dynamic_table_{};
  std::unordered_map<wh::core::any_type_key, wh::core::any,
                     wh::core::any_type_key_hash>
      typed_table_{};
  bool frozen_{false};
};

} // namespace wh::internal
