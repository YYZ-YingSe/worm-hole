#pragma once

#include <any>
#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/internal/merge.hpp"

namespace wh::internal {

using dynamic_stream_chunk = std::any;
using dynamic_stream_chunks = std::span<const dynamic_stream_chunk>;
using dynamic_stream_concat_function =
    std::function<wh::core::result<dynamic_stream_chunk>(dynamic_stream_chunks)>;

namespace detail {

template <typename value_t>
concept adl_stream_concat_available = requires(std::span<const value_t> values) {
  { wh_stream_concat(values) } -> std::same_as<wh::core::result<value_t>>;
};

template <typename value_t>
[[nodiscard]] auto static_stream_concat(std::span<const value_t> values)
    -> wh::core::result<value_t> {
  return wh_stream_concat(values);
}

template <typename map_t>
concept concat_reservable_map_like = requires(map_t &map,
                                              const std::size_t size) {
  { map.reserve(size) };
};

} // namespace detail

class stream_concat_registry {
public:
  stream_concat_registry() = default;

  auto reserve(const std::size_t type_count) -> void {
    table_.reserve(type_count);
    typed_table_.reserve(type_count);
  }

  auto freeze() noexcept -> void {
    frozen_ = true;
  }

  [[nodiscard]] auto is_frozen() const noexcept -> bool {
    return frozen_;
  }

  template <typename value_t>
  auto register_concat(std::function<wh::core::result<value_t>(
                           std::span<const value_t>)>
                           function) -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (!static_cast<bool>(function)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    const auto type = std::type_index(typeid(value_t));
    if (table_.contains(type)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    using typed_concat_fn = typed_concat_fn_t<value_t>;
    auto typed_function =
        std::make_shared<typed_concat_fn>(std::move(function));
    auto [typed_iter, typed_inserted] =
        typed_table_.emplace(type, typed_function);
    if (!typed_inserted) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    auto [dynamic_iter, dynamic_inserted] =
        table_.emplace(
            type, [typed_function](const dynamic_stream_chunks values)
                  -> wh::core::result<dynamic_stream_chunk> {
              std::vector<value_t> typed_values;
              typed_values.reserve(values.size());
              for (const auto &value : values) {
                const auto *typed = std::any_cast<value_t>(&value);
                if (typed == nullptr) {
                  return wh::core::result<dynamic_stream_chunk>::failure(
                      wh::core::errc::type_mismatch);
                }
                typed_values.push_back(*typed);
              }

              auto merged = (*typed_function)(typed_values);
              if (merged.has_error()) {
                return wh::core::result<dynamic_stream_chunk>::failure(
                    merged.error());
              }

              return dynamic_stream_chunk{std::move(merged).value()};
            });
    if (!dynamic_inserted) {
      typed_table_.erase(typed_iter);
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }
    static_cast<void>(dynamic_iter);

    return {};
  }

  template <typename value_t>
  auto register_concat_from_ptrs(
      std::function<wh::core::result<value_t>(std::span<const value_t *>)>
          function) -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (!static_cast<bool>(function)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    auto typed_function = [function](const std::span<const value_t> values)
                              -> wh::core::result<value_t> {
      wh::core::small_vector<const value_t *, 8U> typed_values;
      const auto reserved = typed_values.reserve(
          static_cast<typename decltype(typed_values)::size_type>(values.size()));
      if (reserved.has_error()) {
        return wh::core::result<value_t>::failure(reserved.error());
      }
      for (const auto &value : values) {
        const auto appended = typed_values.push_back(std::addressof(value));
        if (appended.has_error()) {
          return wh::core::result<value_t>::failure(appended.error());
        }
      }
      return function(std::span<const value_t *>{typed_values.data(),
                                                 typed_values.size()});
    };

    const auto type = std::type_index(typeid(value_t));
    if (table_.contains(type)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    using typed_concat_fn = typed_concat_fn_t<value_t>;
    auto typed_function_holder =
        std::make_shared<typed_concat_fn>(std::move(typed_function));
    auto [typed_iter, typed_inserted] =
        typed_table_.emplace(type, typed_function_holder);
    if (!typed_inserted) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    auto [dynamic_iter, dynamic_inserted] =
        table_.emplace(
            type, [function = std::move(function)](
                      const dynamic_stream_chunks values)
                      -> wh::core::result<dynamic_stream_chunk> {
              wh::core::small_vector<const value_t *, 8U> typed_values;
              const auto reserved = typed_values.reserve(
                  static_cast<typename decltype(typed_values)::size_type>(
                      values.size()));
              if (reserved.has_error()) {
                return wh::core::result<dynamic_stream_chunk>::failure(
                    reserved.error());
              }
              for (const auto &value : values) {
                const auto *typed = std::any_cast<value_t>(&value);
                if (typed == nullptr) {
                  return wh::core::result<dynamic_stream_chunk>::failure(
                      wh::core::errc::type_mismatch);
                }
                const auto appended = typed_values.push_back(typed);
                if (appended.has_error()) {
                  return wh::core::result<dynamic_stream_chunk>::failure(
                      appended.error());
                }
              }

              auto merged = function(std::span<const value_t *>{typed_values.data(),
                                                                typed_values.size()});
              if (merged.has_error()) {
                return wh::core::result<dynamic_stream_chunk>::failure(
                    merged.error());
              }

              return dynamic_stream_chunk{std::move(merged).value()};
            });
    if (!dynamic_inserted) {
      typed_table_.erase(typed_iter);
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }
    static_cast<void>(dynamic_iter);

    return {};
  }

  [[nodiscard]] auto find_concat(const std::type_index type) const noexcept
      -> const dynamic_stream_concat_function * {
    const auto iter = table_.find(type);
    if (iter == table_.end()) {
      return nullptr;
    }
    return &iter->second;
  }

  [[nodiscard]] auto concat(const std::type_index type,
                            const dynamic_stream_chunks values) const
      -> wh::core::result<dynamic_stream_chunk> {
    if (values.empty()) {
      return wh::core::result<dynamic_stream_chunk>::failure(
          wh::core::errc::invalid_argument);
    }

    if (const auto *function = find_concat(type); function != nullptr) {
      return (*function)(values);
    }

    if (values.size() == 1U) {
      if (std::type_index(values.front().type()) != type) {
        return wh::core::result<dynamic_stream_chunk>::failure(
            wh::core::errc::type_mismatch);
      }
      return values.front();
    }

    return wh::core::result<dynamic_stream_chunk>::failure(
        wh::core::errc::contract_violation);
  }

  template <typename value_t>
  [[nodiscard]] auto concat_as(const std::span<const value_t> values) const
      -> wh::core::result<value_t> {
    if (values.empty()) {
      return wh::core::result<value_t>::failure(wh::core::errc::invalid_argument);
    }

    if constexpr (detail::adl_stream_concat_available<value_t>) {
      return detail::static_stream_concat(values);
    }

    if (const auto *typed_function = find_typed_concat<value_t>();
        typed_function != nullptr) {
      return (*typed_function)(values);
    }

    if (values.size() == 1U) {
      return values.front();
    }

    if constexpr (std::same_as<std::remove_cv_t<value_t>, std::string>) {
      std::string merged;
      std::size_t total_size = 0U;
      for (const auto &piece : values) {
        total_size += piece.size();
      }
      merged.reserve(total_size);
      for (const auto &piece : values) {
        merged += piece;
      }
      return merged;
    } else if constexpr (std::is_arithmetic_v<value_t>) {
      return values.back();
    } else if constexpr (string_keyed_map_like<value_t>) {
      value_t merged;
      if constexpr (detail::concat_reservable_map_like<value_t>) {
        std::size_t reserve_size = 0U;
        for (const auto &map_value : values) {
          reserve_size += map_value.size();
        }
        merged.reserve(reserve_size);
      }
      for (const auto &map_value : values) {
        for (const auto &[key, item] : map_value) {
          auto [iter, inserted] = merged.try_emplace(key, item);
          if (inserted) {
            continue;
          }

          auto nested = concat_pair<typename value_t::mapped_type>(iter->second,
                                                                   item);
          if (nested.has_error()) {
            return wh::core::result<value_t>::failure(nested.error());
          }
          iter->second = std::move(nested).value();
        }
      }
      return merged;
    }

    return wh::core::result<value_t>::failure(
        wh::core::errc::contract_violation);
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return table_.size();
  }

private:
  template <typename value_t>
  [[nodiscard]] auto concat_pair(const value_t &left, const value_t &right) const
      -> wh::core::result<value_t> {
    if constexpr (detail::adl_stream_concat_available<value_t>) {
      std::array<value_t, 2U> pair_values{left, right};
      return detail::static_stream_concat<value_t>(
          std::span<const value_t>{pair_values});
    }

    if (const auto *typed_function = find_typed_concat<value_t>();
        typed_function != nullptr) {
      std::array<value_t, 2U> pair_values{left, right};
      return (*typed_function)(std::span<const value_t>{pair_values});
    }

    if constexpr (std::same_as<std::remove_cv_t<value_t>, std::string>) {
      std::string merged;
      merged.reserve(left.size() + right.size());
      merged += left;
      merged += right;
      return merged;
    } else if constexpr (std::is_arithmetic_v<value_t>) {
      return right;
    } else if constexpr (string_keyed_map_like<value_t>) {
      value_t merged{left};
      if constexpr (detail::concat_reservable_map_like<value_t>) {
        merged.reserve(left.size() + right.size());
      }
      for (const auto &[key, item] : right) {
        auto [iter, inserted] = merged.try_emplace(key, item);
        if (inserted) {
          continue;
        }

        auto nested =
            concat_pair<typename value_t::mapped_type>(iter->second, item);
        if (nested.has_error()) {
          return wh::core::result<value_t>::failure(nested.error());
        }
        iter->second = std::move(nested).value();
      }
      return merged;
    }

    return wh::core::result<value_t>::failure(
        wh::core::errc::contract_violation);
  }

  template <typename value_t>
  using typed_concat_fn_t =
      std::function<wh::core::result<value_t>(std::span<const value_t>)>;

  template <typename value_t>
  [[nodiscard]] auto find_typed_concat() const noexcept
      -> const typed_concat_fn_t<value_t> * {
    const auto iter = typed_table_.find(std::type_index(typeid(value_t)));
    if (iter == typed_table_.end()) {
      return nullptr;
    }

    const auto *holder =
        std::any_cast<const std::shared_ptr<typed_concat_fn_t<value_t>>>(
            &iter->second);
    if (holder == nullptr || !(*holder)) {
      return nullptr;
    }
    return holder->get();
  }

  std::unordered_map<std::type_index, dynamic_stream_concat_function> table_{};
  std::unordered_map<std::type_index, std::any> typed_table_{};
  bool frozen_{false};
};

} // namespace wh::internal
