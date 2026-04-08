// Defines reusable compile-time type traits and concepts used by component,
// function, and container-level generic code paths.
#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "wh/core/result.hpp"

namespace wh::core {

/// Removes cv/ref qualifiers from `t`.
template <typename t> using remove_cvref_t = std::remove_cvref_t<t>;

/// Transparent hash for heterogeneous `std::string` key lookup.
struct transparent_string_hash {
  using is_transparent = void;

  /// Hashes `std::string_view`.
  [[nodiscard]] auto operator()(const std::string_view value) const noexcept
      -> std::size_t {
    return std::hash<std::string_view>{}(value);
  }

  /// Hashes `std::string`.
  [[nodiscard]] auto operator()(const std::string &value) const noexcept
      -> std::size_t {
    return (*this)(std::string_view{value});
  }

  /// Hashes C-string.
  [[nodiscard]] auto operator()(const char *value) const noexcept
      -> std::size_t {
    return (*this)(std::string_view{value});
  }
};

/// Transparent equality comparator for heterogeneous `std::string` key lookup.
struct transparent_string_equal {
  using is_transparent = void;

  /// Compares two key views.
  [[nodiscard]] auto operator()(const std::string_view left,
                                const std::string_view right) const noexcept
      -> bool {
    return left == right;
  }
};

template <typename t>
/// `true` when type exposes value_type and begin/end/size container interface.
concept container_like = requires(remove_cvref_t<t> container) {
  typename remove_cvref_t<t>::value_type;
  container.begin();
  container.end();
  container.size();
};

template <typename t>
/// `true` when type exposes first/second pair-style members and aliases.
concept pair_like = requires(remove_cvref_t<t> value) {
  typename remove_cvref_t<t>::first_type;
  typename remove_cvref_t<t>::second_type;
  value.first;
  value.second;
};

template <typename t> struct is_optional : std::false_type {};

/// `true` for `std::optional<T>`.
template <typename value_t>
struct is_optional<std::optional<value_t>> : std::true_type {};

template <typename t>
inline constexpr bool is_optional_v = is_optional<remove_cvref_t<t>>::value;

template <typename t> struct is_unique_ptr : std::false_type {};

/// `true` for `std::unique_ptr<T, Deleter>`.
template <typename value_t, typename deleter_t>
struct is_unique_ptr<std::unique_ptr<value_t, deleter_t>> : std::true_type {};

template <typename t>
inline constexpr bool is_unique_ptr_v = is_unique_ptr<remove_cvref_t<t>>::value;

template <typename t> struct is_shared_ptr : std::false_type {};

/// `true` for `std::shared_ptr<T>`.
template <typename value_t>
struct is_shared_ptr<std::shared_ptr<value_t>> : std::true_type {};

template <typename t>
inline constexpr bool is_shared_ptr_v = is_shared_ptr<remove_cvref_t<t>>::value;

template <typename t>
inline constexpr bool is_raw_pointer_v = std::is_pointer_v<remove_cvref_t<t>>;

template <typename t>
inline constexpr bool is_pointer_like_v =
    is_raw_pointer_v<t> || is_unique_ptr_v<t> || is_shared_ptr_v<t>;

template <typename callable_t, typename... args_t>
/// `true` when callable can be invoked with `args_t...`.
concept callable_with = std::invocable<callable_t, args_t...>;

template <typename callable_t, typename... args_t>
/// Invoke result type alias for `callable_t(args_t...)`.
using callable_result_t = std::invoke_result_t<callable_t, args_t...>;

template <typename t> struct is_result : std::false_type {};

/// `true` for `result<T, E>`.
template <typename value_t, typename error_t>
struct is_result<result<value_t, error_t>> : std::true_type {};

template <typename t>
inline constexpr bool is_result_v = is_result<remove_cvref_t<t>>::value;

template <typename t>
/// `true` for `wh::core::result<T, E>` values.
concept result_like = is_result_v<t>;

} // namespace wh::core
