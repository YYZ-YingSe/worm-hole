#pragma once

#include <concepts>
#include <functional>
#include <ostream>
#include <type_traits>
#include <utility>
#include <variant>

#include "wh/core/compiler.hpp"
#include "wh/core/error.hpp"

namespace wh::core {

template <typename value_t, typename error_t> class result;

namespace detail {

template <typename type_t>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<type_t>>;

template <typename value_t, typename error_t>
using result_storage_t = std::variant<value_t, error_t>;

template <typename from_t, typename to_t>
struct is_value_convertible_to : std::is_convertible<from_t, to_t> {};

template <typename from_t, typename to_t>
struct is_value_convertible_to<from_t, to_t &>
    : std::bool_constant<
          std::is_lvalue_reference_v<from_t> &&
          std::is_convertible_v<std::remove_reference_t<from_t> *, to_t *>> {};

template <typename maybe_result_t> struct is_result : std::false_type {};

template <typename value_t, typename error_t>
struct is_result<result<value_t, error_t>> : std::true_type {};

template <typename maybe_result_t>
concept result_like = is_result<remove_cvref_t<maybe_result_t>>::value;

template <typename... args_t>
inline constexpr bool is_single_errc_pack_v =
    (sizeof...(args_t) == 1U) &&
    (std::same_as<remove_cvref_t<args_t>, errc> && ...);

template <typename value_t, typename arg_t>
struct reference_to_temporary
    : std::bool_constant<
          !std::is_lvalue_reference_v<arg_t> ||
          !std::convertible_to<std::remove_reference_t<arg_t> *, value_t *>> {};

template <typename value_t, typename arg_t>
inline constexpr bool reference_to_temporary_v =
    reference_to_temporary<value_t, arg_t>::value;

} // namespace detail

using in_place_value_t = std::in_place_index_t<0>;
inline constexpr in_place_value_t in_place_value{};

using in_place_error_t = std::in_place_index_t<1>;
inline constexpr in_place_error_t in_place_error{};

template <typename value_t> struct success_type {
  using value_type = value_t;

  value_t payload;

  [[nodiscard]] constexpr auto value() & noexcept -> value_t & {
    return payload;
  }

  [[nodiscard]] constexpr auto value() const & noexcept -> const value_t & {
    return payload;
  }

  [[nodiscard]] constexpr auto value() && noexcept -> value_t && {
    return std::move(payload);
  }
};

template <> struct success_type<void> {
  using value_type = void;
};

template <typename error_t> struct failure_type {
  using error_type = error_t;

  error_t payload;

  [[nodiscard]] constexpr auto error() & noexcept -> error_t & {
    return payload;
  }

  [[nodiscard]] constexpr auto error() const & noexcept -> const error_t & {
    return payload;
  }

  [[nodiscard]] constexpr auto error() && noexcept -> error_t && {
    return std::move(payload);
  }
};

[[nodiscard]] constexpr auto success() noexcept -> success_type<void> {
  return success_type<void>{};
}

template <typename value_t>
[[nodiscard]] constexpr auto success(value_t &&value)
    -> success_type<detail::remove_cvref_t<value_t>> {
  return success_type<detail::remove_cvref_t<value_t>>{
      std::forward<value_t>(value)};
}

template <typename error_t>
[[nodiscard]] constexpr auto failure(error_t &&error)
    -> failure_type<detail::remove_cvref_t<error_t>> {
  return failure_type<detail::remove_cvref_t<error_t>>{
      std::forward<error_t>(error)};
}

template <typename value_t, typename error_t = error_code> class result {
public:
  using value_type = value_t;
  using error_type = error_t;

  static_assert(!std::is_reference_v<value_t>,
                "result value type cannot be a reference");
  static_assert(!std::is_reference_v<error_t>,
                "result error type cannot be a reference");

  static constexpr in_place_value_t in_place_value{};
  static constexpr in_place_error_t in_place_error{};

  constexpr result() noexcept(std::is_nothrow_default_constructible_v<value_t>)
    requires std::default_initializable<value_t>
      : storage_(std::in_place_index<0>) {}

  template <typename value_u = value_t>
    requires std::convertible_to<value_u, value_t> &&
             (!std::constructible_from<error_t, value_u &&>) &&
             (!detail::is_single_errc_pack_v<value_u> ||
              !std::is_arithmetic_v<value_t>) &&
             (!std::same_as<detail::remove_cvref_t<value_u>, result>)
  constexpr result(value_u &&value) noexcept(
      std::is_nothrow_constructible_v<value_t, value_u &&>)
      : storage_(std::in_place_index<0>, std::forward<value_u>(value)) {}

  template <typename error_u = error_t>
    requires std::convertible_to<error_u, error_t> &&
             (!std::constructible_from<value_t, error_u &&>) &&
             (!std::same_as<detail::remove_cvref_t<error_u>, result>)
  constexpr result(error_u &&error) noexcept(
      std::is_nothrow_constructible_v<error_t, error_u &&>)
      : storage_(std::in_place_index<1>, std::forward<error_u>(error)) {}

  template <typename... args_t>
    requires std::constructible_from<value_t, args_t &&...> &&
             (!((std::convertible_to<args_t, value_t> && ...) &&
                (sizeof...(args_t) == 1U))) &&
             (!std::constructible_from<error_t, args_t && ...>) &&
             (sizeof...(args_t) >= 1U)
  constexpr explicit result(args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<value_t, args_t &&...>)
      : storage_(std::in_place_index<0>, std::forward<args_t>(args)...) {}

  template <typename... args_t>
    requires(!std::constructible_from<value_t, args_t && ...>) &&
            std::constructible_from<error_t, args_t &&...> &&
            (sizeof...(args_t) >= 1U)
  constexpr explicit result(args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<error_t, args_t &&...>)
      : storage_(std::in_place_index<1>, std::forward<args_t>(args)...) {}

  result(const result &) = default;
  result(result &&) noexcept = default;
  auto operator=(const result &) -> result & = default;
  auto operator=(result &&) noexcept -> result & = default;
  ~result() = default;

  template <typename... args_t>
    requires std::constructible_from<value_t, args_t &&...>
  constexpr result(in_place_value_t, args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<value_t, args_t &&...>)
      : storage_(std::in_place_index<0>, std::forward<args_t>(args)...) {}

  template <typename... args_t>
    requires std::constructible_from<error_t, args_t &&...>
  constexpr result(in_place_error_t, args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<error_t, args_t &&...>)
      : storage_(std::in_place_index<1>, std::forward<args_t>(args)...) {}

  template <typename value_u, typename error_u>
    requires std::convertible_to<value_u, value_t> &&
             std::convertible_to<error_u, error_t> &&
             (!std::convertible_to<const result<value_u, error_u> &, value_t>)
  constexpr result(const result<value_u, error_u> &other)
      : storage_(copy_convert_storage(other)) {}

  template <typename value_u, typename error_u>
    requires std::convertible_to<value_u, value_t> &&
             std::convertible_to<error_u, error_t> &&
             (!std::convertible_to<result<value_u, error_u> &&, value_t>)
  constexpr result(result<value_u, error_u> &&other)
      : storage_(move_convert_storage(std::move(other))) {}

  template <typename value_u = value_t>
    requires std::constructible_from<value_t, value_u &&>
  [[nodiscard]] static constexpr auto success(value_u &&value) -> result {
    return result(in_place_value, std::forward<value_u>(value));
  }

  template <typename error_u = error_t>
    requires std::constructible_from<error_t, error_u &&>
  [[nodiscard]] static constexpr auto failure(error_u &&error) -> result {
    return result(in_place_error, std::forward<error_u>(error));
  }

  template <typename value_u>
    requires std::constructible_from<value_t, value_u &&>
  constexpr explicit result(success_type<value_u> ok)
      : storage_(std::in_place_index<0>, std::forward<value_u>(ok.payload)) {}

  constexpr explicit result(success_type<void>)
    requires std::default_initializable<value_t>
      : storage_(std::in_place_index<0>) {}

  template <typename error_u>
    requires std::constructible_from<error_t, error_u &&>
  constexpr explicit result(failure_type<error_u> err)
      : storage_(std::in_place_index<1>, std::forward<error_u>(err.payload)) {}

  [[nodiscard]] constexpr auto has_value() const noexcept -> bool {
    return storage_.index() == 0U;
  }

  [[nodiscard]] constexpr auto has_error() const noexcept -> bool {
    return storage_.index() == 1U;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return has_value();
  }

  [[nodiscard]] constexpr auto value() & -> value_t & {
    wh_precondition(has_value());
    return std::get<0>(storage_);
  }

  [[nodiscard]] constexpr auto value() const & -> const value_t & {
    wh_precondition(has_value());
    return std::get<0>(storage_);
  }

  [[nodiscard]] constexpr auto
  value() && -> std::conditional_t<std::move_constructible<value_t>, value_t,
                                   value_t &&> {
    wh_precondition(has_value());
    if constexpr (std::move_constructible<value_t>) {
      return static_cast<value_t &&>(std::get<0>(storage_));
    } else {
      return static_cast<value_t &&>(std::get<0>(storage_));
    }
  }

  template <typename value_u = value_t>
  [[nodiscard]] constexpr auto value() const && -> value_t
    requires std::move_constructible<value_u>
  = delete;

  template <typename value_u = value_t>
  [[nodiscard]] constexpr auto value() const && -> const value_t &&requires(
      !std::move_constructible<value_u>) {
    wh_precondition(has_value());
    return static_cast<const value_t &&>(std::get<0>(storage_));
  }

  [[nodiscard]] constexpr auto operator->() noexcept -> value_t * {
    return std::get_if<0>(&storage_);
  }

  [[nodiscard]] constexpr auto operator->() const noexcept -> const value_t * {
    return std::get_if<0>(&storage_);
  }

  [[nodiscard]] constexpr auto operator*() & noexcept -> value_t & {
    wh_precondition(has_value());
    return *operator->();
  }

  [[nodiscard]] constexpr auto operator*() const & noexcept -> const value_t & {
    wh_precondition(has_value());
    return *operator->();
  }

  [[nodiscard]] constexpr auto
  operator*() && noexcept(std::is_nothrow_move_constructible_v<value_t>)
      -> std::conditional_t<std::move_constructible<value_t>, value_t,
                            value_t &&> {
    wh_precondition(has_value());
    if constexpr (std::move_constructible<value_t>) {
      return std::move(**this);
    } else {
      return std::move(**this);
    }
  }

  template <typename value_u = value_t>
  [[nodiscard]] constexpr auto operator*() const && -> value_t
    requires std::move_constructible<value_u>
  = delete;

  template <typename value_u = value_t>
  [[nodiscard]] constexpr auto operator*() const && noexcept
      -> const value_t &&requires(!std::move_constructible<value_u>) {
        wh_precondition(has_value());
        return std::move(**this);
      }

  [[nodiscard]] constexpr auto error() const & noexcept(
      std::is_nothrow_default_constructible_v<error_t> &&
      std::is_nothrow_copy_constructible_v<error_t>) -> error_t {
    return has_error() ? std::get<1>(storage_) : error_t{};
  }

  [[nodiscard]] constexpr auto
  error() && noexcept(std::is_nothrow_default_constructible_v<error_t> &&
                      std::is_nothrow_move_constructible_v<error_t>)
      -> error_t {
    return has_error() ? std::move(std::get<1>(storage_)) : error_t{};
  }

  [[nodiscard]] constexpr auto assume_value() & noexcept -> value_t & {
    return std::get<0>(storage_);
  }

  [[nodiscard]] constexpr auto assume_value() const & noexcept
      -> const value_t & {
    return std::get<0>(storage_);
  }

  [[nodiscard]] constexpr auto assume_value() && noexcept -> value_t && {
    return std::move(std::get<0>(storage_));
  }

  [[nodiscard]] constexpr auto assume_error() & noexcept -> error_t & {
    return std::get<1>(storage_);
  }

  [[nodiscard]] constexpr auto assume_error() const & noexcept
      -> const error_t & {
    return std::get<1>(storage_);
  }

  [[nodiscard]] constexpr auto assume_error() && noexcept -> error_t && {
    return std::move(std::get<1>(storage_));
  }

  template <typename... args_t>
  constexpr auto emplace(args_t &&...args) -> value_t & {
    return storage_.template emplace<0>(std::forward<args_t>(args)...);
  }

  constexpr void
  swap(result &other) noexcept(noexcept(storage_.swap(other.storage_))) {
    storage_.swap(other.storage_);
  }

  friend constexpr void swap(result &lhs,
                             result &rhs) noexcept(noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
  }

  [[nodiscard]] friend constexpr auto
  operator==(const result &lhs,
             const result &rhs) noexcept(noexcept(lhs.storage_ == rhs.storage_))
      -> bool {
    return lhs.storage_ == rhs.storage_;
  }

  [[nodiscard]] friend constexpr auto
  operator!=(const result &lhs,
             const result &rhs) noexcept(noexcept(!(lhs == rhs))) -> bool {
    return !(lhs == rhs);
  }

  template <typename fallback_t>
    requires std::convertible_to<fallback_t, value_t>
  [[nodiscard]] constexpr auto
  value_or(fallback_t &&fallback) const & -> value_t {
    if (has_value()) {
      return std::get<0>(storage_);
    }
    return static_cast<value_t>(std::forward<fallback_t>(fallback));
  }

  template <typename fallback_t>
    requires std::convertible_to<fallback_t, value_t>
  [[nodiscard]] constexpr auto value_or(fallback_t &&fallback) && -> value_t {
    if (has_value()) {
      return std::move(std::get<0>(storage_));
    }
    return static_cast<value_t>(std::forward<fallback_t>(fallback));
  }

private:
  template <typename value_u, typename error_u>
  [[nodiscard]] static constexpr auto
  copy_convert_storage(const result<value_u, error_u> &other)
      -> detail::result_storage_t<value_t, error_t> {
    if (other.has_value()) {
      return detail::result_storage_t<value_t, error_t>(std::in_place_index<0>,
                                                        *other);
    }

    return detail::result_storage_t<value_t, error_t>(std::in_place_index<1>,
                                                      other.error());
  }

  template <typename value_u, typename error_u>
  [[nodiscard]] static constexpr auto
  move_convert_storage(result<value_u, error_u> &&other)
      -> detail::result_storage_t<value_t, error_t> {
    if (other.has_value()) {
      return detail::result_storage_t<value_t, error_t>(std::in_place_index<0>,
                                                        *std::move(other));
    }

    return detail::result_storage_t<value_t, error_t>(std::in_place_index<1>,
                                                      std::move(other).error());
  }

  detail::result_storage_t<value_t, error_t> storage_;
};

template <typename char_t, typename traits_t, typename value_t,
          typename error_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream,
                const result<value_t, error_t> &item)
    -> std::basic_ostream<char_t, traits_t> & {
  if (item.has_value()) {
    stream << "value:" << *item;
  } else {
    stream << "error:" << item.error();
  }

  return stream;
}

template <typename value_t, typename error_t> class result<value_t &, error_t> {
public:
  using value_type = value_t &;
  using error_type = error_t;

  static_assert(!std::is_reference_v<error_t>,
                "result error type cannot be a reference");

  static constexpr in_place_value_t in_place_value{};
  static constexpr in_place_error_t in_place_error{};

  template <typename arg_t>
    requires std::convertible_to<arg_t, value_t &> &&
             (!detail::reference_to_temporary_v<value_t, arg_t>) &&
             (!std::convertible_to<arg_t, error_t>)
  constexpr result(arg_t &&arg) noexcept(
      std::is_nothrow_constructible_v<value_t &, arg_t>)
      : storage_(std::in_place_index<0>, &static_cast<value_t &>(arg)) {}

  template <typename error_u = error_t>
    requires std::convertible_to<error_u, error_t> &&
             (!std::convertible_to<error_u, value_t &>) &&
             (!std::same_as<detail::remove_cvref_t<error_u>, result>)
  constexpr result(error_u &&error) noexcept(
      std::is_nothrow_constructible_v<error_t, error_u &&>)
      : storage_(std::in_place_index<1>, std::forward<error_u>(error)) {}

  template <typename arg_t>
    requires std::constructible_from<value_t &, arg_t &&> &&
             (!std::convertible_to<arg_t, value_t &>) &&
             (!detail::reference_to_temporary_v<value_t, arg_t>) &&
             (!std::constructible_from<error_t, arg_t &&>)
  constexpr explicit result(arg_t &&arg) noexcept(
      std::is_nothrow_constructible_v<value_t &, arg_t>)
      : storage_(std::in_place_index<0>, &static_cast<value_t &>(arg)) {}

  template <typename... args_t>
    requires(!std::constructible_from<value_t &, args_t && ...>) &&
            std::constructible_from<error_t, args_t &&...> &&
            (sizeof...(args_t) >= 1U)
  constexpr explicit result(args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<error_t, args_t &&...>)
      : storage_(std::in_place_index<1>, std::forward<args_t>(args)...) {}

  result(const result &) = default;
  result(result &&) noexcept = default;
  auto operator=(const result &) -> result & = default;
  auto operator=(result &&) noexcept -> result & = default;
  ~result() = default;

  template <typename arg_t>
    requires std::constructible_from<value_t &, arg_t &&> &&
             (!detail::reference_to_temporary_v<value_t, arg_t>)
  constexpr result(in_place_value_t, arg_t &&arg) noexcept(
      std::is_nothrow_constructible_v<value_t &, arg_t>)
      : storage_(std::in_place_index<0>, &static_cast<value_t &>(arg)) {}

  template <typename... args_t>
    requires std::constructible_from<error_t, args_t &&...>
  constexpr result(in_place_error_t, args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<error_t, args_t &&...>)
      : storage_(std::in_place_index<1>, std::forward<args_t>(args)...) {}

  template <typename value_u, typename error_u>
    requires std::convertible_to<value_u &, value_t &> &&
             (!detail::reference_to_temporary_v<value_t, value_u &>) &&
             std::convertible_to<error_u, error_t>
  constexpr result(const result<value_u &, error_u> &other)
      : storage_(copy_convert_storage(other)) {}

  template <typename value_u, typename error_u>
    requires std::convertible_to<value_u &, value_t &> &&
             (!detail::reference_to_temporary_v<value_t, value_u &>) &&
             std::convertible_to<error_u, error_t>
  constexpr result(result<value_u &, error_u> &&other)
      : storage_(move_convert_storage(std::move(other))) {}

  [[nodiscard]] static constexpr auto success(value_t &value) -> result {
    return result(in_place_value, value);
  }

  template <typename error_u = error_t>
    requires std::constructible_from<error_t, error_u &&>
  [[nodiscard]] static constexpr auto failure(error_u &&error) -> result {
    return result(in_place_error, std::forward<error_u>(error));
  }

  [[nodiscard]] constexpr auto has_value() const noexcept -> bool {
    return storage_.index() == 0U;
  }

  [[nodiscard]] constexpr auto has_error() const noexcept -> bool {
    return storage_.index() == 1U;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return has_value();
  }

  [[nodiscard]] constexpr auto value() const -> value_t & {
    wh_precondition(has_value());
    return *std::get<0>(storage_);
  }

  [[nodiscard]] constexpr auto operator->() const noexcept -> value_t * {
    if (const auto *value = std::get_if<0>(&storage_); value != nullptr) {
      return *value;
    }

    return nullptr;
  }

  [[nodiscard]] constexpr auto operator*() const noexcept -> value_t & {
    wh_precondition(has_value());
    return *operator->();
  }

  [[nodiscard]] constexpr auto
  error() const & noexcept(std::is_nothrow_default_constructible_v<error_t> &&
                           std::is_nothrow_copy_constructible_v<error_t>)
      -> error_t {
    return has_error() ? std::get<1>(storage_) : error_t{};
  }

  [[nodiscard]] constexpr auto
  error() && noexcept(std::is_nothrow_default_constructible_v<error_t> &&
                      std::is_nothrow_move_constructible_v<error_t>)
      -> error_t {
    return has_error() ? std::move(std::get<1>(storage_)) : error_t{};
  }

  [[nodiscard]] constexpr auto assume_value() const noexcept -> value_t & {
    return *std::get<0>(storage_);
  }

  [[nodiscard]] constexpr auto assume_error() & noexcept -> error_t & {
    return std::get<1>(storage_);
  }

  [[nodiscard]] constexpr auto assume_error() const & noexcept
      -> const error_t & {
    return std::get<1>(storage_);
  }

  template <typename arg_t>
    requires(!detail::reference_to_temporary_v<value_t, arg_t>)
  constexpr auto emplace(arg_t &&arg) -> value_t & {
    return *storage_.template emplace<0>(&static_cast<value_t &>(arg));
  }

  constexpr void
  swap(result &other) noexcept(noexcept(storage_.swap(other.storage_))) {
    storage_.swap(other.storage_);
  }

  friend constexpr void swap(result &lhs,
                             result &rhs) noexcept(noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
  }

  [[nodiscard]] friend constexpr auto
  operator==(const result &lhs,
             const result &rhs) noexcept(noexcept(lhs.storage_ == rhs.storage_))
      -> bool {
    return lhs.storage_ == rhs.storage_;
  }

  [[nodiscard]] friend constexpr auto
  operator!=(const result &lhs,
             const result &rhs) noexcept(noexcept(!(lhs == rhs))) -> bool {
    return !(lhs == rhs);
  }

  [[nodiscard]] constexpr auto value_or(value_t &fallback) const -> value_t & {
    if (has_value()) {
      return value();
    }

    return fallback;
  }

private:
  template <typename value_u, typename error_u>
  [[nodiscard]] static constexpr auto
  copy_convert_storage(const result<value_u &, error_u> &other)
      -> std::variant<value_t *, error_t> {
    if (other.has_value()) {
      return std::variant<value_t *, error_t>(std::in_place_index<0>,
                                              &other.value());
    }

    return std::variant<value_t *, error_t>(std::in_place_index<1>,
                                            other.error());
  }

  template <typename value_u, typename error_u>
  [[nodiscard]] static constexpr auto
  move_convert_storage(result<value_u &, error_u> &&other)
      -> std::variant<value_t *, error_t> {
    if (other.has_value()) {
      return std::variant<value_t *, error_t>(std::in_place_index<0>,
                                              &other.value());
    }

    return std::variant<value_t *, error_t>(std::in_place_index<1>,
                                            std::move(other).error());
  }

  std::variant<value_t *, error_t> storage_;
};

template <typename char_t, typename traits_t, typename value_t,
          typename error_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream,
                const result<value_t &, error_t> &item)
    -> std::basic_ostream<char_t, traits_t> & {
  if (item.has_value()) {
    stream << "value:" << *item;
  } else {
    stream << "error:" << item.error();
  }

  return stream;
}

template <typename error_t> class result<void, error_t> {
public:
  using value_type = void;
  using error_type = error_t;

  static_assert(!std::is_reference_v<error_t>,
                "result error type cannot be a reference");

  static constexpr in_place_value_t in_place_value{};
  static constexpr in_place_error_t in_place_error{};

  constexpr result() noexcept : storage_(std::in_place_index<0>) {}

  template <typename error_u>
    requires std::constructible_from<error_t, error_u &&> &&
             (!std::convertible_to<error_u, error_t>) &&
             (!std::same_as<detail::remove_cvref_t<error_u>, result>)
  constexpr explicit result(error_u &&error) noexcept(
      std::is_nothrow_constructible_v<error_t, error_u &&>)
      : storage_(std::in_place_index<1>, std::forward<error_u>(error)) {}

  template <typename error_u = error_t>
    requires std::convertible_to<error_u, error_t> &&
             (!std::same_as<detail::remove_cvref_t<error_u>, result>)
  constexpr result(error_u &&error) noexcept(
      std::is_nothrow_constructible_v<error_t, error_u &&>)
      : storage_(std::in_place_index<1>, std::forward<error_u>(error)) {}

  template <typename... args_t>
    requires std::constructible_from<error_t, args_t &&...> &&
             (sizeof...(args_t) >= 2U)
  constexpr result(args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<error_t, args_t &&...>)
      : storage_(std::in_place_index<1>, std::forward<args_t>(args)...) {}

  result(const result &) = default;
  result(result &&) noexcept = default;
  auto operator=(const result &) -> result & = default;
  auto operator=(result &&) noexcept -> result & = default;
  ~result() = default;

  constexpr explicit result(in_place_value_t) noexcept
      : storage_(std::in_place_index<0>) {}

  template <typename... args_t>
    requires std::constructible_from<error_t, args_t &&...>
  constexpr result(in_place_error_t, args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<error_t, args_t &&...>)
      : storage_(std::in_place_index<1>, std::forward<args_t>(args)...) {}

  template <typename error_u>
    requires std::convertible_to<error_u, error_t>
  constexpr result(const result<void, error_u> &other)
      : storage_(std::in_place_index<1>, other.error()) {
    if (other.has_value()) {
      emplace();
    }
  }

  template <typename error_u>
    requires std::convertible_to<error_u, error_t>
  constexpr result(result<void, error_u> &&other)
      : storage_(std::in_place_index<1>, std::move(other).error()) {
    if (other.has_value()) {
      emplace();
    }
  }

  [[nodiscard]] static constexpr auto success() -> result {
    return result(in_place_value);
  }

  template <typename error_u = error_t>
    requires std::constructible_from<error_t, error_u &&>
  [[nodiscard]] static constexpr auto failure(error_u &&error) -> result {
    return result(in_place_error, std::forward<error_u>(error));
  }

  constexpr explicit result(success_type<void>) noexcept
      : storage_(std::in_place_index<0>) {}

  template <typename error_u>
    requires std::constructible_from<error_t, error_u &&>
  constexpr explicit result(failure_type<error_u> err)
      : storage_(std::in_place_index<1>, std::forward<error_u>(err.payload)) {}

  [[nodiscard]] constexpr auto has_value() const noexcept -> bool {
    return storage_.index() == 0U;
  }

  [[nodiscard]] constexpr auto has_error() const noexcept -> bool {
    return storage_.index() == 1U;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return has_value();
  }

  constexpr void value() const { wh_precondition(has_value()); }

  [[nodiscard]] constexpr auto operator->() noexcept -> void * {
    return static_cast<void *>(std::get_if<0>(&storage_));
  }

  [[nodiscard]] constexpr auto operator->() const noexcept -> const void * {
    return static_cast<const void *>(std::get_if<0>(&storage_));
  }

  constexpr void operator*() const noexcept { wh_precondition(has_value()); }

  [[nodiscard]] constexpr auto
  error() const & noexcept(std::is_nothrow_default_constructible_v<error_t> &&
                           std::is_nothrow_copy_constructible_v<error_t>)
      -> error_t {
    return has_error() ? std::get<1>(storage_) : error_t{};
  }

  [[nodiscard]] constexpr auto
  error() && noexcept(std::is_nothrow_default_constructible_v<error_t> &&
                      std::is_nothrow_move_constructible_v<error_t>)
      -> error_t {
    return has_error() ? std::move(std::get<1>(storage_)) : error_t{};
  }

  [[nodiscard]] constexpr auto assume_error() & noexcept -> error_t & {
    return std::get<1>(storage_);
  }

  [[nodiscard]] constexpr auto assume_error() const & noexcept
      -> const error_t & {
    return std::get<1>(storage_);
  }

  constexpr void emplace() { storage_.template emplace<0>(); }

  constexpr void
  swap(result &other) noexcept(noexcept(storage_.swap(other.storage_))) {
    storage_.swap(other.storage_);
  }

  friend constexpr void swap(result &lhs,
                             result &rhs) noexcept(noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
  }

  [[nodiscard]] friend constexpr auto
  operator==(const result &lhs,
             const result &rhs) noexcept(noexcept(lhs.storage_ == rhs.storage_))
      -> bool {
    return lhs.storage_ == rhs.storage_;
  }

  [[nodiscard]] friend constexpr auto
  operator!=(const result &lhs,
             const result &rhs) noexcept(noexcept(!(lhs == rhs))) -> bool {
    return !(lhs == rhs);
  }

private:
  std::variant<std::monostate, error_t> storage_;
};

template <typename char_t, typename traits_t, typename error_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream,
                const result<void, error_t> &item)
    -> std::basic_ostream<char_t, traits_t> & {
  if (item.has_value()) {
    stream << "value:void";
  } else {
    stream << "error:" << item.error();
  }

  return stream;
}

template <typename value_t, typename error_t, typename fallback_t>
  requires detail::is_value_convertible_to<fallback_t, value_t>::value &&
           (!std::invocable<fallback_t>)
[[nodiscard]] constexpr auto operator|(const result<value_t, error_t> &item,
                                       fallback_t &&fallback) -> value_t {
  if (item.has_value()) {
    return *item;
  }

  return std::forward<fallback_t>(fallback);
}

template <typename value_t, typename error_t, typename fallback_t>
  requires detail::is_value_convertible_to<fallback_t, value_t>::value &&
           (!std::invocable<fallback_t>)
[[nodiscard]] constexpr auto operator|(result<value_t, error_t> &&item,
                                       fallback_t &&fallback) -> value_t {
  if (item.has_value()) {
    return *std::move(item);
  }

  return std::forward<fallback_t>(fallback);
}

template <typename value_t, typename error_t, typename factory_t,
          typename produced_t = std::invoke_result_t<factory_t>>
  requires std::invocable<factory_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           detail::is_value_convertible_to<produced_t, value_t>::value
[[nodiscard]] constexpr auto operator|(const result<value_t, error_t> &item,
                                       factory_t &&factory) -> value_t {
  if (item.has_value()) {
    return *item;
  }

  return std::invoke(std::forward<factory_t>(factory));
}

template <typename value_t, typename error_t, typename factory_t,
          typename produced_t = std::invoke_result_t<factory_t>>
  requires std::invocable<factory_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           detail::is_value_convertible_to<produced_t, value_t>::value
[[nodiscard]] constexpr auto operator|(result<value_t, error_t> &&item,
                                       factory_t &&factory) -> value_t {
  if (item.has_value()) {
    return *std::move(item);
  }

  return std::invoke(std::forward<factory_t>(factory));
}

template <typename value_t, typename error_t, typename factory_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<factory_t>>>
  requires std::invocable<factory_t> && detail::result_like<produced_t> &&
           detail::is_value_convertible_to<
               value_t, typename produced_t::value_type>::value
[[nodiscard]] constexpr auto operator|(const result<value_t, error_t> &item,
                                       factory_t &&factory) -> produced_t {
  if (item.has_value()) {
    return produced_t(*item);
  }

  return std::invoke(std::forward<factory_t>(factory));
}

template <typename value_t, typename error_t, typename factory_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<factory_t>>>
  requires std::invocable<factory_t> && detail::result_like<produced_t> &&
           detail::is_value_convertible_to<
               value_t, typename produced_t::value_type>::value
[[nodiscard]] constexpr auto operator|(result<value_t, error_t> &&item,
                                       factory_t &&factory) -> produced_t {
  if (item.has_value()) {
    return produced_t(*std::move(item));
  }

  return std::invoke(std::forward<factory_t>(factory));
}

template <typename error_t, typename factory_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<factory_t>>>
  requires std::invocable<factory_t> && detail::result_like<produced_t> &&
           std::same_as<typename produced_t::value_type, void>
[[nodiscard]] constexpr auto operator|(const result<void, error_t> &item,
                                       factory_t &&factory) -> produced_t {
  if (item.has_value()) {
    return produced_t{};
  }

  return std::invoke(std::forward<factory_t>(factory));
}

template <typename error_t, typename factory_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<factory_t>>>
  requires std::invocable<factory_t> && detail::result_like<produced_t> &&
           std::same_as<typename produced_t::value_type, void>
[[nodiscard]] constexpr auto operator|(result<void, error_t> &&item,
                                       factory_t &&factory) -> produced_t {
  if (item.has_value()) {
    return produced_t{};
  }

  return std::invoke(std::forward<factory_t>(factory));
}

template <typename value_t, typename error_t, typename fallback_t>
  requires detail::is_value_convertible_to<fallback_t, value_t>::value &&
           (!std::invocable<fallback_t>)
constexpr auto operator|=(result<value_t, error_t> &item, fallback_t &&fallback)
    -> result<value_t, error_t> & {
  if (!item) {
    item = std::forward<fallback_t>(fallback);
  }

  return item;
}

template <typename value_t, typename error_t, typename factory_t,
          typename produced_t = std::invoke_result_t<factory_t>>
  requires std::invocable<factory_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           detail::is_value_convertible_to<produced_t, value_t>::value
constexpr auto operator|=(result<value_t, error_t> &item, factory_t &&factory)
    -> result<value_t, error_t> & {
  if (!item) {
    item = std::invoke(std::forward<factory_t>(factory));
  }

  return item;
}

template <typename value_t, typename error_t, typename factory_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<factory_t>>>
  requires std::invocable<factory_t> && detail::result_like<produced_t> &&
           detail::is_value_convertible_to<typename produced_t::value_type,
                                           value_t>::value &&
           std::convertible_to<typename produced_t::error_type, error_t>
constexpr auto operator|=(result<value_t, error_t> &item, factory_t &&factory)
    -> result<value_t, error_t> & {
  if (!item) {
    item = std::invoke(std::forward<factory_t>(factory));
  }

  return item;
}

template <
    typename value_t, typename error_t, typename callable_t,
    typename produced_t = std::invoke_result_t<callable_t, const value_t &>>
  requires std::invocable<callable_t, const value_t &> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           (!std::is_void_v<produced_t>)
[[nodiscard]] constexpr auto operator&(const result<value_t, error_t> &item,
                                       callable_t &&callable)
    -> result<produced_t, error_t> {
  if (item.has_error()) {
    return item.error();
  }

  return std::invoke(std::forward<callable_t>(callable), *item);
}

template <typename value_t, typename error_t, typename callable_t,
          typename produced_t = std::invoke_result_t<callable_t, value_t>>
  requires std::invocable<callable_t, value_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           (!std::is_void_v<produced_t>)
[[nodiscard]] constexpr auto operator&(result<value_t, error_t> &&item,
                                       callable_t &&callable)
    -> result<produced_t, error_t> {
  if (item.has_error()) {
    return std::move(item).error();
  }

  return std::invoke(std::forward<callable_t>(callable), *std::move(item));
}

template <
    typename value_t, typename error_t, typename callable_t,
    typename produced_t = std::invoke_result_t<callable_t, const value_t &>>
  requires std::invocable<callable_t, const value_t &> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           std::is_void_v<produced_t>
[[nodiscard]] constexpr auto operator&(const result<value_t, error_t> &item,
                                       callable_t &&callable)
    -> result<void, error_t> {
  if (item.has_error()) {
    return item.error();
  }

  std::invoke(std::forward<callable_t>(callable), *item);
  return {};
}

template <typename value_t, typename error_t, typename callable_t,
          typename produced_t = std::invoke_result_t<callable_t, value_t>>
  requires std::invocable<callable_t, value_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           std::is_void_v<produced_t>
[[nodiscard]] constexpr auto operator&(result<value_t, error_t> &&item,
                                       callable_t &&callable)
    -> result<void, error_t> {
  if (item.has_error()) {
    return std::move(item).error();
  }

  std::invoke(std::forward<callable_t>(callable), *std::move(item));
  return {};
}

template <typename value_t, typename error_t, typename callable_t,
          typename produced_t = detail::remove_cvref_t<
              std::invoke_result_t<callable_t, const value_t &>>>
  requires std::invocable<callable_t, const value_t &> &&
           detail::result_like<produced_t> &&
           std::convertible_to<error_t, typename produced_t::error_type>
[[nodiscard]] constexpr auto operator&(const result<value_t, error_t> &item,
                                       callable_t &&callable) -> produced_t {
  if (item.has_error()) {
    return item.error();
  }

  return std::invoke(std::forward<callable_t>(callable), *item);
}

template <typename value_t, typename error_t, typename callable_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<callable_t, value_t>>>
  requires std::invocable<callable_t, value_t> &&
           detail::result_like<produced_t> &&
           std::convertible_to<error_t, typename produced_t::error_type>
[[nodiscard]] constexpr auto operator&(result<value_t, error_t> &&item,
                                       callable_t &&callable) -> produced_t {
  if (item.has_error()) {
    return std::move(item).error();
  }

  return std::invoke(std::forward<callable_t>(callable), *std::move(item));
}

template <typename error_t, typename callable_t,
          typename produced_t = std::invoke_result_t<callable_t>>
  requires std::invocable<callable_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           (!std::is_void_v<produced_t>)
[[nodiscard]] constexpr auto operator&(const result<void, error_t> &item,
                                       callable_t &&callable)
    -> result<produced_t, error_t> {
  if (item.has_error()) {
    return item.error();
  }

  return std::invoke(std::forward<callable_t>(callable));
}

template <typename error_t, typename callable_t,
          typename produced_t = std::invoke_result_t<callable_t>>
  requires std::invocable<callable_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           std::is_void_v<produced_t>
[[nodiscard]] constexpr auto operator&(const result<void, error_t> &item,
                                       callable_t &&callable)
    -> result<void, error_t> {
  if (item.has_error()) {
    return item.error();
  }

  std::invoke(std::forward<callable_t>(callable));
  return {};
}

template <typename error_t, typename callable_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<callable_t>>>
  requires std::invocable<callable_t> && detail::result_like<produced_t> &&
           std::convertible_to<error_t, typename produced_t::error_type>
[[nodiscard]] constexpr auto operator&(const result<void, error_t> &item,
                                       callable_t &&callable) -> produced_t {
  if (item.has_error()) {
    return item.error();
  }

  return std::invoke(std::forward<callable_t>(callable));
}

template <typename value_t, typename error_t, typename callable_t,
          typename produced_t = std::invoke_result_t<callable_t, value_t>>
  requires std::invocable<callable_t, value_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>) &&
           detail::is_value_convertible_to<produced_t, value_t>::value
constexpr auto operator&=(result<value_t, error_t> &item, callable_t &&callable)
    -> result<value_t, error_t> & {
  if (item) {
    item = std::invoke(std::forward<callable_t>(callable), *std::move(item));
  }

  return item;
}

template <typename error_t, typename callable_t,
          typename produced_t = std::invoke_result_t<callable_t>>
  requires std::invocable<callable_t> &&
           (!detail::result_like<detail::remove_cvref_t<produced_t>>)
constexpr auto operator&=(result<void, error_t> &item, callable_t &&callable)
    -> result<void, error_t> & {
  if (item) {
    (void)std::invoke(std::forward<callable_t>(callable));
  }

  return item;
}

template <typename value_t, typename error_t, typename callable_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<callable_t, value_t>>>
  requires std::invocable<callable_t, value_t> &&
           detail::result_like<produced_t> &&
           detail::is_value_convertible_to<typename produced_t::value_type,
                                           value_t>::value &&
           std::convertible_to<typename produced_t::error_type, error_t>
constexpr auto operator&=(result<value_t, error_t> &item, callable_t &&callable)
    -> result<value_t, error_t> & {
  if (item) {
    item = std::invoke(std::forward<callable_t>(callable), *std::move(item));
  }

  return item;
}

template <typename error_t, typename callable_t,
          typename produced_t =
              detail::remove_cvref_t<std::invoke_result_t<callable_t>>>
  requires std::invocable<callable_t> && detail::result_like<produced_t> &&
           std::same_as<typename produced_t::value_type, void> &&
           std::convertible_to<typename produced_t::error_type, error_t>
constexpr auto operator&=(result<void, error_t> &item, callable_t &&callable)
    -> result<void, error_t> & {
  if (item) {
    item = std::invoke(std::forward<callable_t>(callable));
  }

  return item;
}

} // namespace wh::core
