// Defines stdexec-style manual lifetime storage for sender operation states.
#pragma once

#include <concepts>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace wh::core::detail {

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
    return *std::launder(::new (static_cast<void *>(buffer_))
                             value_t{std::forward<arg_ts>(args)...});
  }

  template <typename factory_t, typename... arg_ts>
  [[nodiscard]] auto construct_from(factory_t &&factory,
                                    arg_ts &&...args) -> value_t & {
    return *std::launder(::new (static_cast<void *>(buffer_))
                             value_t{std::forward<factory_t>(factory)(
                                 std::forward<arg_ts>(args)...)});
  }

  template <typename factory_t>
  [[nodiscard]] auto construct_with(factory_t &&factory) -> value_t & {
    return construct_from(std::forward<factory_t>(factory));
  }

  auto destruct() noexcept -> void { std::destroy_at(std::addressof(get())); }

  [[nodiscard]] auto get() & noexcept -> value_t & {
    return *reinterpret_cast<value_t *>(buffer_);
  }

  [[nodiscard]] auto get() && noexcept -> value_t && {
    return static_cast<value_t &&>(*reinterpret_cast<value_t *>(buffer_));
  }

  [[nodiscard]] auto get() const & noexcept -> const value_t & {
    return *reinterpret_cast<const value_t *>(buffer_);
  }

  [[nodiscard]] auto get() const && noexcept -> const value_t && = delete;

private:
  alignas(value_t) unsigned char buffer_[sizeof(value_t)]{};
};

} // namespace wh::core::detail
