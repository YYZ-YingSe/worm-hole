// Defines stdexec-style manual lifetime storage for sender operation states.
#pragma once

#include <cstddef>
#include <concepts>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace wh::core::detail {

// Untyped storage keeps operation-state member layouts free of internal-linkage
// template arguments while still constructing the value in-place.
template <std::size_t Size, std::size_t Align> class manual_storage {
public:
  manual_storage() noexcept = default;
  ~manual_storage() = default;

  manual_storage(const manual_storage &) = delete;
  auto operator=(const manual_storage &) -> manual_storage & = delete;
  manual_storage(manual_storage &&) = delete;
  auto operator=(manual_storage &&) -> manual_storage & = delete;

  template <typename value_t, typename... arg_ts>
  [[nodiscard]] auto construct(arg_ts &&...args) noexcept(
      std::is_nothrow_constructible_v<value_t, arg_ts...>) -> value_t & {
    validate_type<value_t>();
    return *std::launder(::new (static_cast<void *>(buffer_))
                             value_t{std::forward<arg_ts>(args)...});
  }

  template <typename value_t, typename factory_t, typename... arg_ts>
  [[nodiscard]] auto construct_from(factory_t &&factory,
                                    arg_ts &&...args) -> value_t & {
    validate_type<value_t>();
    return *std::launder(::new (static_cast<void *>(buffer_))
                             value_t{std::forward<factory_t>(factory)(
                                 std::forward<arg_ts>(args)...)});
  }

  template <typename value_t, typename factory_t>
  [[nodiscard]] auto construct_with(factory_t &&factory) -> value_t & {
    return construct_from<value_t>(std::forward<factory_t>(factory));
  }

  template <typename value_t> auto destruct() noexcept -> void {
    std::destroy_at(std::addressof(get<value_t>()));
  }

  template <typename value_t> [[nodiscard]] auto get() & noexcept -> value_t & {
    validate_type<value_t>();
    return *reinterpret_cast<value_t *>(buffer_);
  }

  template <typename value_t> [[nodiscard]] auto get() && noexcept -> value_t && {
    validate_type<value_t>();
    return static_cast<value_t &&>(*reinterpret_cast<value_t *>(buffer_));
  }

  template <typename value_t>
  [[nodiscard]] auto get() const & noexcept -> const value_t & {
    validate_type<value_t>();
    return *reinterpret_cast<const value_t *>(buffer_);
  }

  template <typename value_t>
  [[nodiscard]] auto get() const && noexcept -> const value_t && = delete;

private:
  template <typename value_t> static consteval auto validate_type() -> void {
    static_assert(std::is_object_v<value_t>,
                  "manual_storage only supports object types");
    static_assert(sizeof(value_t) <= Size,
                  "manual_storage buffer is too small for value type");
    static_assert(alignof(value_t) <= Align,
                  "manual_storage alignment is too small for value type");
  }

  alignas(Align) std::byte buffer_[Size]{};
};

template <typename value_t> class manual_lifetime {
public:
  manual_lifetime() noexcept = default;
  ~manual_lifetime() = default;

  manual_lifetime(const manual_lifetime &) = delete;
  auto operator=(const manual_lifetime &) -> manual_lifetime & = delete;
  manual_lifetime(manual_lifetime &&) = delete;
  auto operator=(manual_lifetime &&) -> manual_lifetime & = delete;

  template <typename... arg_ts>
  [[nodiscard]] auto construct(arg_ts &&...args) noexcept(
      std::is_nothrow_constructible_v<value_t, arg_ts...>) -> value_t & {
    return storage_.template construct<value_t>(std::forward<arg_ts>(args)...);
  }

  template <typename factory_t, typename... arg_ts>
  [[nodiscard]] auto construct_from(factory_t &&factory,
                                    arg_ts &&...args) -> value_t & {
    return storage_.template construct_from<value_t>(
        std::forward<factory_t>(factory), std::forward<arg_ts>(args)...);
  }

  template <typename factory_t>
  [[nodiscard]] auto construct_with(factory_t &&factory) -> value_t & {
    return storage_.template construct_with<value_t>(
        std::forward<factory_t>(factory));
  }

  auto destruct() noexcept -> void { storage_.template destruct<value_t>(); }

  [[nodiscard]] auto get() & noexcept -> value_t & {
    return storage_.template get<value_t>();
  }

  [[nodiscard]] auto get() && noexcept -> value_t && {
    return static_cast<value_t &&>(storage_.template get<value_t>());
  }

  [[nodiscard]] auto get() const & noexcept -> const value_t & {
    return storage_.template get<value_t>();
  }

  [[nodiscard]] auto get() const && noexcept -> const value_t && = delete;

private:
  manual_storage<sizeof(value_t), alignof(value_t)> storage_{};
};

} // namespace wh::core::detail
