// Defines generic type utilities for default construction helpers,
// pointer-like normalization, and container transformation operations.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <memory>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/core/type_traits.hpp"
#include "wh/internal/type_name.hpp"

namespace wh::core {

template <typename t> struct type_tag {
  using type = t;
};

template <typename t>
using type_of_t = typename type_tag<remove_cvref_t<t>>::type;

/// Produces a compile-time type tag with cvref removed.
template <typename t>
[[nodiscard]] consteval auto type_of() noexcept -> type_tag<remove_cvref_t<t>> {
  return {};
}

template <typename t>
concept default_initializable_object =
    std::default_initializable<remove_cvref_t<t>>;

template <typename t>
/// Returns whether pointer-like holder currently points to an instance.
[[nodiscard]] constexpr inline auto has_instance(const t &value) noexcept
    -> bool {
  if constexpr (is_raw_pointer_v<t>) {
    return value != nullptr;
  } else if constexpr (is_unique_ptr_v<t> || is_shared_ptr_v<t>) {
    return static_cast<bool>(value);
  } else {
    return true;
  }
}

template <typename t>
/// Returns mutable reference to pointed value for pointer-like holders.
[[nodiscard]] constexpr inline auto deref_or_self(t &value) noexcept
    -> decltype(auto) {
  if constexpr (is_pointer_like_v<t>) {
    return *value;
  } else {
    return (value);
  }
}

template <typename t>
/// Returns const reference to pointed value for pointer-like holders.
[[nodiscard]] constexpr inline auto deref_or_self(const t &value) noexcept
    -> decltype(auto) {
  if constexpr (is_pointer_like_v<t>) {
    return *value;
  } else {
    return (value);
  }
}

template <typename... ts> struct type_list {};

template <typename list_t> struct type_list_size;

/// Compile-time size for `type_list`.
template <typename... ts>
struct type_list_size<type_list<ts...>>
    : std::integral_constant<std::size_t, sizeof...(ts)> {};

template <std::size_t index, typename list_t> struct type_list_at;

/// Type lookup at non-zero index.
template <std::size_t index, typename head_t, typename... tail_t>
struct type_list_at<index, type_list<head_t, tail_t...>>
    : type_list_at<index - 1U, type_list<tail_t...>> {};

/// Type lookup specialization for index 0.
template <typename head_t, typename... tail_t>
struct type_list_at<0U, type_list<head_t, tail_t...>> {
  using type = head_t;
};

template <typename list_t> struct type_list_reverse;

template <> struct type_list_reverse<type_list<>> {
  using type = type_list<>;
};

/// Compile-time reverse of `type_list`.
template <typename head_t, typename... tail_t>
struct type_list_reverse<type_list<head_t, tail_t...>> {
private:
  using tail_reversed = typename type_list_reverse<type_list<tail_t...>>::type;

  template <typename reversed_t> struct append_head;

  /// Appends current head to reversed tail.
  template <typename... reversed_items_t>
  struct append_head<type_list<reversed_items_t...>> {
    using type = type_list<reversed_items_t..., head_t>;
  };

public:
  using type = typename append_head<tail_reversed>::type;
};

template <typename t> struct default_instance_factory {
  /// Creates a default instance with error-safe result wrapping.
  [[nodiscard]] static auto make() -> result<remove_cvref_t<t>>
    requires default_initializable_object<t>
  {
    return remove_cvref_t<t>{};
  }
};

template <typename t> struct default_instance_factory<t *> {
  /// Creates pointee then heap-allocates raw pointer result.
  [[nodiscard]] static auto make() -> result<t *> {
    using pointee_t = std::remove_cv_t<t>;
    auto nested = default_instance_factory<pointee_t>::make();
    if (nested.has_error()) {
      return result<t *>::failure(nested.error());
    }

    auto value = std::make_unique<pointee_t>(std::move(nested.value()));
    return value.release();
  }
};

/// Creates a default instance wrapped in `result`.
template <typename t>
[[nodiscard]] auto default_instance() -> result<remove_cvref_t<t>> {
  return default_instance_factory<remove_cvref_t<t>>::make();
}

/// Wraps value into `std::unique_ptr` with error-safe allocation handling.
template <typename value_t>
[[nodiscard]] auto wrap_unique(value_t &&value)
    -> result<std::unique_ptr<remove_cvref_t<value_t>>> {
  using normalized_t = remove_cvref_t<value_t>;
  return std::make_unique<normalized_t>(std::forward<value_t>(value));
}

/// Copies input sequence into reversed `std::vector`.
template <typename sequence_t>
[[nodiscard]] auto reverse_copy(const sequence_t &sequence)
    -> result<std::vector<typename sequence_t::value_type>> {
  std::vector<typename sequence_t::value_type> output;
  output.reserve(sequence.size());
  [[maybe_unused]] const auto copied =
      std::ranges::reverse_copy(sequence, std::back_inserter(output));

  return output;
}

/// Copies key/value pairs into a target map-like container.
template <typename map_out_t, typename map_in_t>
[[nodiscard]] auto map_copy_as(const map_in_t &input) -> result<map_out_t> {
  map_out_t output;
  for (const auto &[key, value] : input) {
    output.insert_or_assign(key, value);
  }

  return output;
}

template <typename t>
[[nodiscard]] constexpr std::string_view stable_type_token() noexcept {
  return ::wh::internal::persistent_type_alias<t>();
}

template <typename t>
[[nodiscard]] constexpr std::string_view diagnostic_type_token() noexcept {
  return ::wh::internal::diagnostic_type_alias<t>();
}

} // namespace wh::core
