// Defines callable storage containers (inline/heap) used by function
// wrappers to hold targets with small-buffer optimization.
#pragma once

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace wh::core::fn_detail {

/// Raw aligned storage used by function wrappers for vtable/object payload.
template <std::size_t size_v> union any_storage {
  /// Pointer sentinel used by empty-state checks.
  void *head_{nullptr};
  /// Backing byte storage for in-place object construction.
  alignas(void *) std::byte data_[std::max<std::size_t>(size_v, 1U)];

  any_storage() noexcept = default;

  any_storage(std::nullptr_t) noexcept : head_(nullptr) {}

  auto operator=(std::nullptr_t) noexcept -> any_storage & {
    head_ = nullptr;
    return *this;
  }

  /// Returns writable storage address.
  [[nodiscard]] auto address() noexcept -> void * { return &data_[0]; }

  /// Returns readonly storage address.
  [[nodiscard]] auto address() const noexcept -> const void * {
    return &data_[0];
  }

  /// Returns pointer slot used for empty-state tracking.
  [[nodiscard]] auto head() noexcept -> void *& { return head_; }

  /// Returns pointer slot used for empty-state tracking.
  [[nodiscard]] auto head() const noexcept -> void *const & { return head_; }

  /// Interprets storage as a const-qualified target type.
  template <typename type_t>
    requires std::is_const_v<std::remove_reference_t<type_t>>
  [[nodiscard]] auto interpret_as() const noexcept -> type_t {
    static_assert(sizeof(type_t) <= size_v,
                  "any_storage is smaller than the requested object!");
    using stored_t = std::decay_t<type_t>;
    return std::forward<type_t>(*static_cast<const stored_t *>(address()));
  }

  /// Interprets storage as a mutable target type.
  template <typename type_t>
  [[nodiscard]] auto interpret_as() noexcept -> type_t {
    using const_type_t = const std::remove_reference_t<type_t> &;
    return const_cast<type_t &&>(
        (static_cast<const any_storage &>(*this)).interpret_as<const_type_t>());
  }

  /// Returns whether the requested type can fit in this storage.
  template <typename type_t>
  [[nodiscard]] static consteval auto can_construct() -> bool {
    return (sizeof(type_t) <= size_v) && (alignof(type_t) <= alignof(void *)) &&
           (alignof(void *) % alignof(type_t) == 0U);
  }
};

} // namespace wh::core::fn_detail
