// Defines SBO-backed type erasure with owning and borrowed policies.
#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include "wh/core/result.hpp"
#include "wh/internal/type_name.hpp"

namespace wh::core {

namespace detail {

template <typename value_t> struct has_tuple_size_value : std::false_type {};

template <typename value_t>
  requires requires { std::tuple_size<const value_t>::value; }
struct has_tuple_size_value<value_t> : std::true_type {};

template <typename value_t> struct has_value_type : std::false_type {};

template <typename value_t>
  requires requires { typename value_t::value_type; }
struct has_value_type<value_t> : std::true_type {};

template <typename value_t>
[[nodiscard]] consteval auto maybe_equality_comparable(int)
    -> decltype(std::declval<const value_t &>() == std::declval<const value_t &>(), bool{}) {
  return true;
}

template <typename> [[nodiscard]] consteval auto maybe_equality_comparable(char) -> bool {
  return false;
}

template <typename value_t> [[nodiscard]] consteval auto dispatch_is_equality_comparable() -> bool;

template <typename value_t, std::size_t... index>
[[nodiscard]] consteval auto unpack_maybe_equality_comparable(std::index_sequence<index...>)
    -> bool {
  return (dispatch_is_equality_comparable<std::tuple_element_t<index, value_t>>() && ...);
}

template <typename value_t> [[nodiscard]] consteval auto dispatch_is_equality_comparable() -> bool {
  if constexpr (std::is_array_v<value_t>) {
    return false;
  } else if constexpr (has_tuple_size_value<value_t>::value) {
    return maybe_equality_comparable<value_t>(0) &&
           unpack_maybe_equality_comparable<value_t>(
               std::make_index_sequence<std::tuple_size_v<value_t>>{});
  } else if constexpr (has_value_type<value_t>::value) {
    if constexpr (std::same_as<typename value_t::value_type, value_t> ||
                  dispatch_is_equality_comparable<typename value_t::value_type>()) {
      return maybe_equality_comparable<value_t>(0);
    } else {
      return false;
    }
  } else {
    return maybe_equality_comparable<value_t>(0);
  }
}

template <typename value_t>
inline constexpr bool any_value_equal_enabled_v = dispatch_is_equality_comparable<value_t>();

template <typename value_t>
[[nodiscard]] inline auto any_value_equal(const value_t &left, const value_t &right) noexcept
    -> bool {
  return left == right;
}

} // namespace detail

/// Lightweight runtime type key used by `basic_any`.
struct any_type_key {
  const void *token{nullptr};

  [[nodiscard]] constexpr auto operator==(const any_type_key &) const noexcept -> bool = default;
};

/// Hash functor for `any_type_key`.
struct any_type_key_hash {
  [[nodiscard]] auto operator()(const any_type_key key) const noexcept -> std::size_t {
    return std::hash<const void *>{}(key.token);
  }
};

/// Lightweight runtime metadata describing one `basic_any` payload type.
struct any_type_info {
  any_type_key key{};
  std::string_view name{};
  std::size_t size{};
  std::size_t alignment{};
  const std::type_info &(*type)() noexcept;

  [[nodiscard]] constexpr auto operator==(const any_type_info &) const noexcept -> bool = default;
};

/// Storage policy of one `basic_any` instance.
enum class any_policy : std::uint8_t {
  empty = 0U,
  inline_owner,
  heap_owner,
  ref,
  cref,
};

/// Custom ownerization policy used by `basic_any::into_owned()`.
///
/// `into_owned(const T&)` is the non-consuming path.
/// Implementations must not mutate `value`; failure leaves the source unchanged.
///
/// `into_owned(T&&)` is the consuming path.
/// Implementations should prefer move/transfer semantics when possible.
/// On failure the source object must remain valid for destruction/assignment,
/// but its observable state is not required to match the pre-call state.
template <typename value_t> struct any_owned_traits {};

namespace detail {

template <typename value_t>
concept has_any_owned_copy = requires(const value_t &value) {
  { any_owned_traits<value_t>::into_owned(value) } -> std::same_as<wh::core::result<value_t>>;
};

template <typename value_t>
concept has_any_owned_move = requires(value_t &value) {
  {
    any_owned_traits<value_t>::into_owned(std::move(value))
  } -> std::same_as<wh::core::result<value_t>>;
};

template <typename value_t>
inline constexpr bool has_custom_any_owned_v =
    has_any_owned_copy<value_t> || has_any_owned_move<value_t>;

template <typename value_t>
[[nodiscard]] inline auto copy_into_owned_value(const value_t &value) -> wh::core::result<value_t> {
  if constexpr (has_any_owned_copy<value_t>) {
    return any_owned_traits<value_t>::into_owned(value);
  } else if constexpr (std::copy_constructible<value_t>) {
    return value_t{value};
  } else {
    return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
  }
}

template <typename value_t>
[[nodiscard]] inline auto into_owned_value(value_t &value) -> wh::core::result<value_t> {
  if constexpr (has_any_owned_move<value_t>) {
    return any_owned_traits<value_t>::into_owned(std::move(value));
  } else if constexpr (std::move_constructible<value_t>) {
    return value_t{std::move(value)};
  } else if constexpr (has_any_owned_copy<value_t>) {
    return any_owned_traits<value_t>::into_owned(std::as_const(value));
  } else if constexpr (std::copy_constructible<value_t>) {
    return value_t{value};
  } else {
    return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
  }
}

} // namespace detail

template <std::size_t inline_size = 3U * sizeof(void *),
          std::size_t inline_align = alignof(std::max_align_t)>
class basic_any {
private:
  template <typename value_t>
  static constexpr bool fits_inline_v =
      (inline_size != 0U) && (alignof(value_t) <= inline_align) &&
      (sizeof(value_t) <= inline_size) && std::is_nothrow_move_constructible_v<value_t>;

  static constexpr std::size_t storage_size = (inline_size == 0U) ? 1U : inline_size;

  union storage_t {
    alignas(inline_align) std::byte buffer[storage_size];
    const void *ptr;

    constexpr storage_t() noexcept : ptr(nullptr) {}
  };

  struct vtable_t {
    const any_type_info *info{};
    void (*destroy)(basic_any &) noexcept;
    void (*copy_construct)(const basic_any &, basic_any &);
    void (*move_construct)(basic_any &, basic_any &) noexcept;
    error_code (*copy_into_owned)(const basic_any &, basic_any &);
    error_code (*consume_into_owned)(basic_any &, basic_any &);
    bool (*assign_copy)(basic_any &, const void *);
    bool (*assign_move)(basic_any &, void *);
    bool (*equal)(const void *, const void *) noexcept;
    bool copyable;
  };

  template <typename value_t> struct model final {
    static inline constexpr int token = 0;

    [[nodiscard]] static constexpr auto key() noexcept -> any_type_key {
      return any_type_key{&token};
    }

    [[nodiscard]] static auto type() noexcept -> const std::type_info & { return typeid(value_t); }

    static inline constexpr any_type_info info{
        .key = any_type_key{&token},
        .name = ::wh::internal::stable_type_name<value_t>(),
        .size = sizeof(value_t),
        .alignment = alignof(value_t),
        .type = &model::type,
    };

    static auto destroy(basic_any &self) noexcept -> void {
      switch (self.policy_) {
      case any_policy::inline_owner:
        if constexpr (!std::is_trivially_destructible_v<value_t>) {
          inline_ptr(self)->~value_t();
        }
        break;
      case any_policy::heap_owner:
        delete heap_ptr(self);
        self.storage_.ptr = nullptr;
        break;
      case any_policy::empty:
      case any_policy::ref:
      case any_policy::cref:
        break;
      }
    }

    static auto copy_construct(const basic_any &source, basic_any &target) -> void {
      if constexpr (std::copy_constructible<value_t>) {
        if constexpr (fits_inline_v<value_t>) {
          ::new (static_cast<void *>(target.storage_.buffer)) value_t{*const_ptr(source)};
          target.policy_ = any_policy::inline_owner;
        } else {
          target.storage_.ptr = new value_t{*const_ptr(source)};
          target.policy_ = any_policy::heap_owner;
        }
      }
    }

    static auto move_construct(basic_any &source, basic_any &target) noexcept -> void {
      if constexpr (fits_inline_v<value_t>) {
        auto *typed = inline_ptr(source);
        ::new (static_cast<void *>(target.storage_.buffer)) value_t{std::move(*typed)};
        if constexpr (!std::is_trivially_destructible_v<value_t>) {
          typed->~value_t();
        }
        target.policy_ = any_policy::inline_owner;
        source.policy_ = any_policy::empty;
      }
    }

    static auto assign_copy(basic_any &target, const void *source) -> bool {
      if constexpr (std::is_copy_assignable_v<value_t>) {
        *mutable_ptr(target) = *static_cast<const value_t *>(source);
        return true;
      } else {
        (void)target;
        (void)source;
        return false;
      }
    }

    static auto assign_move(basic_any &target, void *source) -> bool {
      if constexpr (std::is_move_assignable_v<value_t>) {
        *mutable_ptr(target) = std::move(*static_cast<value_t *>(source));
        return true;
      } else if constexpr (std::is_copy_assignable_v<value_t>) {
        *mutable_ptr(target) = *static_cast<const value_t *>(source);
        return true;
      } else {
        (void)target;
        (void)source;
        return false;
      }
    }

    static auto equal(const void *lhs, const void *rhs) noexcept -> bool {
      if constexpr (detail::any_value_equal_enabled_v<value_t>) {
        return detail::any_value_equal(*static_cast<const value_t *>(lhs),
                                       *static_cast<const value_t *>(rhs));
      } else {
        return lhs == rhs;
      }
    }

    static auto copy_into_owned(const basic_any &source, basic_any &target) -> error_code {
      auto cloned = detail::copy_into_owned_value<value_t>(*const_ptr(source));
      if (cloned.has_error()) {
        return cloned.error();
      }
      target.template initialize<value_t>(std::move(cloned).value());
      return {};
    }

    static auto consume_into_owned(basic_any &source, basic_any &target) -> error_code {
      if (source.is_borrowed()) {
        return copy_into_owned(source, target);
      }

      if constexpr (detail::has_custom_any_owned_v<value_t>) {
        auto cloned = detail::into_owned_value<value_t>(*mutable_ptr(source));
        if (cloned.has_error()) {
          return cloned.error();
        }
        target.template initialize<value_t>(std::move(cloned).value());
        source.reset();
        return {};
      }

      target.move_from(std::move(source));
      return {};
    }

    [[nodiscard]] static auto mutable_ptr(basic_any &self) noexcept -> value_t * {
      if (self.policy_ == any_policy::inline_owner) {
        return inline_ptr(self);
      }
      return heap_ptr(self);
    }

    [[nodiscard]] static auto const_ptr(const basic_any &self) noexcept -> const value_t * {
      switch (self.policy_) {
      case any_policy::inline_owner:
        return inline_ptr(self);
      case any_policy::heap_owner:
      case any_policy::ref:
      case any_policy::cref:
        return static_cast<const value_t *>(self.storage_.ptr);
      case any_policy::empty:
        return nullptr;
      }
      return nullptr;
    }

    [[nodiscard]] static auto inline_ptr(basic_any &self) noexcept -> value_t * {
      return std::launder(reinterpret_cast<value_t *>(self.storage_.buffer));
    }

    [[nodiscard]] static auto inline_ptr(const basic_any &self) noexcept -> const value_t * {
      return std::launder(reinterpret_cast<const value_t *>(self.storage_.buffer));
    }

    [[nodiscard]] static auto heap_ptr(basic_any &self) noexcept -> value_t * {
      return static_cast<value_t *>(const_cast<void *>(self.storage_.ptr));
    }

    static inline constexpr vtable_t vtable{
        .info = &model::info,
        .destroy = &model::destroy,
        .copy_construct = std::copy_constructible<value_t> ? &model::copy_construct : nullptr,
        .move_construct = &model::move_construct,
        .copy_into_owned = &model::copy_into_owned,
        .consume_into_owned = &model::consume_into_owned,
        .assign_copy = &model::assign_copy,
        .assign_move = &model::assign_move,
        .equal = &model::equal,
        .copyable = std::copy_constructible<value_t>,
    };
  };

  template <typename value_t, typename... arg_ts> auto initialize(arg_ts &&...args) -> void {
    using stored_t = std::remove_cvref_t<value_t>;
    if constexpr (std::is_void_v<value_t>) {
      return;
    } else if constexpr (std::is_lvalue_reference_v<value_t>) {
      static_assert(sizeof...(arg_ts) == 1U,
                    "reference initialization requires exactly one argument");
      storage_.ptr = (std::addressof(args), ...);
      vtable_ = &model<stored_t>::vtable;
      if constexpr (std::is_const_v<std::remove_reference_t<value_t>>) {
        policy_ = any_policy::cref;
      } else {
        policy_ = any_policy::ref;
      }
    } else if constexpr (fits_inline_v<stored_t>) {
      ::new (static_cast<void *>(storage_.buffer)) stored_t(std::forward<arg_ts>(args)...);
      vtable_ = &model<stored_t>::vtable;
      policy_ = any_policy::inline_owner;
    } else {
      storage_.ptr = new stored_t(std::forward<arg_ts>(args)...);
      vtable_ = &model<stored_t>::vtable;
      policy_ = any_policy::heap_owner;
    }
  }

public:
  template <typename value_t>
  [[nodiscard]] static constexpr auto type_key() noexcept -> any_type_key {
    return model<std::remove_cvref_t<value_t>>::key();
  }

  template <typename value_t>
  [[nodiscard]] static constexpr auto info_of() noexcept -> const any_type_info & {
    return model<std::remove_cvref_t<value_t>>::info;
  }

  static constexpr std::size_t length = inline_size;
  static constexpr std::size_t alignment = inline_align;

  /// Creates an empty container.
  basic_any() noexcept = default;

  /// Stores a decayed owned value.
  template <typename value_t>
    requires(!std::same_as<std::remove_cvref_t<value_t>, basic_any>)
  basic_any(value_t &&value) {
    initialize<std::decay_t<value_t>>(std::forward<value_t>(value));
  }

  /// Constructs an owned value in place.
  template <typename value_t, typename... arg_ts>
  explicit basic_any(std::in_place_type_t<value_t>, arg_ts &&...args) {
    initialize<value_t>(std::forward<arg_ts>(args)...);
  }

  /// Takes ownership of one heap object allocated by the caller.
  template <typename value_t>
    requires(!std::is_const_v<value_t> && !std::is_void_v<value_t>)
  explicit basic_any(std::in_place_t, value_t *value) noexcept {
    if (value == nullptr) {
      return;
    }
    storage_.ptr = value;
    vtable_ = &model<value_t>::vtable;
    policy_ = any_policy::heap_owner;
  }

  /// Copies one container. Borrowed states stay borrowed; non-copyable owners
  /// become empty.
  basic_any(const basic_any &other) { copy_from(other); }

  /// Moves one container and transfers its policy.
  basic_any(basic_any &&other) noexcept { move_from(std::move(other)); }

  auto operator=(const basic_any &other) -> basic_any & {
    if (this != &other) {
      reset();
      copy_from(other);
    }
    return *this;
  }

  auto operator=(basic_any &&other) noexcept -> basic_any & {
    if (this != &other) {
      reset();
      move_from(std::move(other));
    }
    return *this;
  }

  template <typename value_t>
    requires(!std::same_as<std::remove_cvref_t<value_t>, basic_any>)
  auto operator=(value_t &&value) -> basic_any & {
    emplace<std::decay_t<value_t>>(std::forward<value_t>(value));
    return *this;
  }

  ~basic_any() { reset(); }

  auto swap(basic_any &other) noexcept -> void {
    if (this == &other) {
      return;
    }
    if (policy_ != any_policy::inline_owner && other.policy_ != any_policy::inline_owner) {
      std::swap(storage_.ptr, other.storage_.ptr);
      std::swap(vtable_, other.vtable_);
      std::swap(policy_, other.policy_);
      return;
    }
    basic_any tmp{std::move(other)};
    other = std::move(*this);
    *this = std::move(tmp);
  }

  /// Creates an owned inline/heap value depending on size and move traits.
  template <typename value_t, typename... arg_ts> auto emplace(arg_ts &&...args) -> decltype(auto) {
    using stored_t = std::remove_cvref_t<value_t>;
    reset();
    initialize<value_t>(std::forward<arg_ts>(args)...);
    if constexpr (!std::is_void_v<value_t>) {
      return *data<stored_t>();
    }
  }

  /// Aliases one mutable object without taking ownership.
  template <typename value_t> [[nodiscard]] static auto ref(value_t &value) noexcept -> basic_any {
    basic_any current{};
    current.storage_.ptr = std::addressof(value);
    current.vtable_ = &model<std::remove_cvref_t<value_t>>::vtable;
    current.policy_ = any_policy::ref;
    return current;
  }

  /// Aliases one const object without taking ownership.
  template <typename value_t>
  [[nodiscard]] static auto cref(const value_t &value) noexcept -> basic_any {
    basic_any current{};
    current.storage_.ptr = std::addressof(value);
    current.vtable_ = &model<std::remove_cvref_t<value_t>>::vtable;
    current.policy_ = any_policy::cref;
    return current;
  }

  /// Creates a borrowed alias of the current payload.
  [[nodiscard]] auto as_ref() noexcept -> basic_any {
    switch (policy_) {
    case any_policy::empty:
      return {};
    case any_policy::cref:
      return std::as_const(*this).as_ref();
    case any_policy::ref:
      return ref_from_pointer(storage_.ptr, vtable_);
    case any_policy::inline_owner:
    case any_policy::heap_owner:
      return ref_from_pointer(data(), vtable_);
    }
    return {};
  }

  /// Creates a const borrowed alias of the current payload.
  [[nodiscard]] auto as_ref() const noexcept -> basic_any {
    if (policy_ == any_policy::empty) {
      return {};
    }
    return cref_from_pointer(data(), vtable_);
  }

  /// Reassigns the contained object when the target is mutable and types match.
  [[nodiscard]] auto assign(const basic_any &other) -> bool {
    if (!compatible_assign(other)) {
      return false;
    }
    return vtable_->assign_copy != nullptr && vtable_->assign_copy(*this, other.data());
  }

  /// Reassigns the contained object using move semantics when possible.
  [[nodiscard]] auto assign(basic_any &&other) -> bool {
    if (!compatible_assign(other)) {
      return false;
    }
    if (other.policy_ == any_policy::cref) {
      return vtable_->assign_copy != nullptr && vtable_->assign_copy(*this, other.data());
    }
    return vtable_->assign_move != nullptr && vtable_->assign_move(*this, other.data());
  }

  /// Reassigns from one typed value when the contained type matches exactly.
  template <typename value_t> [[nodiscard]] auto assign(value_t &&value) -> bool {
    using stored_t = std::remove_cvref_t<value_t>;
    if (!has_value<stored_t>() || policy_ == any_policy::cref) {
      return false;
    }
    if constexpr (std::is_lvalue_reference_v<value_t> || !std::is_move_assignable_v<stored_t>) {
      return vtable_->assign_copy != nullptr && vtable_->assign_copy(*this, std::addressof(value));
    } else {
      return vtable_->assign_move != nullptr && vtable_->assign_move(*this, std::addressof(value));
    }
  }

  auto reset() noexcept -> void {
    if (vtable_ != nullptr) {
      vtable_->destroy(*this);
    }
    vtable_ = nullptr;
    policy_ = any_policy::empty;
    storage_.ptr = nullptr;
  }

  [[nodiscard]] auto has_value() const noexcept -> bool { return policy_ != any_policy::empty; }

  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  template <typename value_t> [[nodiscard]] auto has_value() const noexcept -> bool {
    return vtable_ == &model<std::remove_cvref_t<value_t>>::vtable;
  }

  [[nodiscard]] auto has_value(const any_type_key query) const noexcept -> bool {
    return key() == query;
  }

  [[nodiscard]] auto has_value(const any_type_info &query) const noexcept -> bool {
    return has_value(query.key);
  }

  [[nodiscard]] auto copyable() const noexcept -> bool {
    return vtable_ != nullptr && (is_borrowed() || vtable_->copyable);
  }

  [[nodiscard]] auto owner() const noexcept -> bool {
    return policy_ == any_policy::inline_owner || policy_ == any_policy::heap_owner;
  }

  [[nodiscard]] auto borrowed() const noexcept -> bool {
    return policy_ == any_policy::ref || policy_ == any_policy::cref;
  }

  [[nodiscard]] auto policy() const noexcept -> any_policy { return policy_; }

  [[nodiscard]] auto key() const noexcept -> any_type_key { return info().key; }

  [[nodiscard]] auto type() const noexcept -> const std::type_info & { return info().type(); }

  [[nodiscard]] auto info() const noexcept -> const any_type_info & {
    if (vtable_ != nullptr) {
      return *vtable_->info;
    }
    return empty_info();
  }

  [[nodiscard]] auto data() const noexcept -> const void * {
    switch (policy_) {
    case any_policy::empty:
      return nullptr;
    case any_policy::inline_owner:
      return storage_.buffer;
    case any_policy::heap_owner:
    case any_policy::ref:
    case any_policy::cref:
      return storage_.ptr;
    }
    return nullptr;
  }

  [[nodiscard]] auto data() noexcept -> void * {
    if (policy_ == any_policy::cref) {
      return nullptr;
    }
    return const_cast<void *>(std::as_const(*this).data());
  }

  [[nodiscard]] auto data(const any_type_key query) const noexcept -> const void * {
    return has_value(query) ? data() : nullptr;
  }

  [[nodiscard]] auto data(const any_type_key query) noexcept -> void * {
    return has_value(query) ? data() : nullptr;
  }

  [[nodiscard]] auto data(const any_type_info &query) const noexcept -> const void * {
    return data(query.key);
  }

  [[nodiscard]] auto data(const any_type_info &query) noexcept -> void * { return data(query.key); }

  template <typename value_t> [[nodiscard]] auto data() noexcept -> value_t * {
    using stored_t = std::remove_const_t<value_t>;
    if (vtable_ != &model<stored_t>::vtable || policy_ == any_policy::cref) {
      return nullptr;
    }
    return model<stored_t>::mutable_ptr(*this);
  }

  template <typename value_t> [[nodiscard]] auto data() const noexcept -> const value_t * {
    using stored_t = std::remove_const_t<value_t>;
    if (vtable_ != &model<stored_t>::vtable) {
      return nullptr;
    }
    return model<stored_t>::const_ptr(*this);
  }

  template <typename value_t> [[nodiscard]] auto get_if() noexcept -> value_t * {
    return data<value_t>();
  }

  template <typename value_t> [[nodiscard]] auto get_if() const noexcept -> const value_t * {
    return data<value_t>();
  }

  [[nodiscard]] auto operator==(const basic_any &other) const noexcept -> bool {
    if (vtable_ != other.vtable_) {
      return !has_value() && !other.has_value();
    }
    if (!has_value()) {
      return true;
    }
    return vtable_->equal != nullptr && vtable_->equal(data(), other.data());
  }

  /// Materializes an owned payload without consuming the source object.
  [[nodiscard]] auto into_owned() const & -> wh::core::result<basic_any>;

  /// Consumes the current container and returns an owned payload.
  /// On success `*this` becomes empty. On failure `*this` remains valid but its
  /// payload state is unspecified when the underlying ownerization path
  /// consumed subobjects.
  [[nodiscard]] auto into_owned() && -> wh::core::result<basic_any>;

private:
  [[nodiscard]] static auto ref_from_pointer(const void *ptr, const vtable_t *vtable) noexcept
      -> basic_any {
    basic_any current{};
    current.storage_.ptr = ptr;
    current.vtable_ = vtable;
    current.policy_ = any_policy::ref;
    return current;
  }

  [[nodiscard]] static auto cref_from_pointer(const void *ptr, const vtable_t *vtable) noexcept
      -> basic_any {
    basic_any current{};
    current.storage_.ptr = ptr;
    current.vtable_ = vtable;
    current.policy_ = any_policy::cref;
    return current;
  }

  [[nodiscard]] auto compatible_assign(const basic_any &other) const noexcept -> bool {
    return has_value() && other.has_value() && vtable_ == other.vtable_ &&
           policy_ != any_policy::cref;
  }

  [[nodiscard]] auto is_borrowed() const noexcept -> bool {
    return policy_ == any_policy::ref || policy_ == any_policy::cref;
  }

  [[nodiscard]] static auto empty_info() noexcept -> const any_type_info & {
    static const any_type_info current{
        .key = {},
        .name = ::wh::internal::stable_type_name<void>(),
        .size = 0U,
        .alignment = 0U,
        .type = []() noexcept -> const std::type_info & { return typeid(void); },
    };
    return current;
  }

  auto copy_from(const basic_any &other) -> void {
    if (other.vtable_ == nullptr) {
      return;
    }
    if (other.is_borrowed()) {
      storage_.ptr = other.storage_.ptr;
      vtable_ = other.vtable_;
      policy_ = other.policy_;
      return;
    }
    if (!other.vtable_->copyable) {
      return;
    }
    other.vtable_->copy_construct(other, *this);
    vtable_ = other.vtable_;
  }

  auto move_from(basic_any &&other) noexcept -> void {
    if (other.vtable_ == nullptr) {
      return;
    }
    if (other.policy_ != any_policy::inline_owner) {
      storage_.ptr = std::exchange(other.storage_.ptr, nullptr);
      vtable_ = std::exchange(other.vtable_, nullptr);
      policy_ = std::exchange(other.policy_, any_policy::empty);
      return;
    }
    other.vtable_->move_construct(other, *this);
    vtable_ = std::exchange(other.vtable_, nullptr);
    if (policy_ == any_policy::empty) {
      vtable_ = nullptr;
    }
  }

  storage_t storage_{};
  const vtable_t *vtable_{nullptr};
  any_policy policy_{any_policy::empty};
};

template <std::size_t inline_size, std::size_t inline_align>
auto basic_any<inline_size, inline_align>::into_owned() const & -> wh::core::result<basic_any> {
  if (!has_value()) {
    return basic_any{};
  }

  basic_any cloned{};
  const auto status = vtable_->copy_into_owned(*this, cloned);
  if (status.failed()) {
    return wh::core::result<basic_any>::failure(status);
  }
  return cloned;
}

template <std::size_t inline_size, std::size_t inline_align>
auto basic_any<inline_size, inline_align>::into_owned() && -> wh::core::result<basic_any> {
  if (!has_value()) {
    return basic_any{};
  }

  basic_any cloned{};
  const auto status = vtable_->consume_into_owned(*this, cloned);
  if (status.failed()) {
    return wh::core::result<basic_any>::failure(status);
  }
  return cloned;
}

template <std::size_t inline_size, std::size_t inline_align>
struct any_owned_traits<basic_any<inline_size, inline_align>> {
  [[nodiscard]] static auto into_owned(const basic_any<inline_size, inline_align> &value)
      -> wh::core::result<basic_any<inline_size, inline_align>> {
    return value.into_owned();
  }

  [[nodiscard]] static auto into_owned(basic_any<inline_size, inline_align> &&value)
      -> wh::core::result<basic_any<inline_size, inline_align>> {
    return std::move(value).into_owned();
  }
};

template <typename value_t>
[[nodiscard]] inline auto into_owned(const value_t &value)
    -> wh::core::result<std::remove_cvref_t<value_t>> {
  using stored_t = std::remove_cvref_t<value_t>;
  return detail::copy_into_owned_value<stored_t>(value);
}

template <typename value_t>
  requires(!std::is_lvalue_reference_v<value_t>)
[[nodiscard]] inline auto into_owned(value_t &&value)
    -> wh::core::result<std::remove_cvref_t<value_t>> {
  using stored_t = std::remove_cvref_t<value_t>;
  auto &slot = value;
  return detail::into_owned_value<stored_t>(slot);
}

template <std::size_t inline_size, std::size_t inline_align>
auto swap(basic_any<inline_size, inline_align> &lhs,
          basic_any<inline_size, inline_align> &rhs) noexcept -> void {
  lhs.swap(rhs);
}

} // namespace wh::core
