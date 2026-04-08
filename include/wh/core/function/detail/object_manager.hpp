// Defines object lifecycle management primitives used by function wrappers
// for construction, move, destroy, and target access.
#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

#include "wh/core/function/detail/error_policy.hpp"
#include "wh/core/function/detail/ownership_policy.hpp"
#include "wh/core/function/detail/storage.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::core::fn_detail {

/// Shared type and trait aliases for object manager policies.
template <
    typename target_t,
    template <typename, template <typename> class> class ownership_template,
    std::size_t buffer_size, template <typename> class allocator_t>
class manager_helper {
protected:
  using max_buffer = any_storage<buffer_size>;

  static constexpr bool using_soo =
      max_buffer::template can_construct<target_t>();

public:
  using ownership_policy =
      std::conditional_t<using_soo, local_ownership<target_t, allocator_t>,
                         ownership_template<target_t, allocator_t>>;

  using allocator_type = typename ownership_policy::allocator_type;
  using stored_type = typename ownership_policy::stored_type;
  using buffer_type = any_storage<sizeof(stored_type)>;

  template <typename... args_t>
  static constexpr bool is_constructible =
      buffer_type::template can_construct<stored_type>() &&
      std::is_constructible_v<std::decay_t<target_t>, args_t...>;

  static constexpr bool is_copy_constructible =
      buffer_type::template can_construct<stored_type>() &&
      std::is_copy_constructible_v<std::decay_t<target_t>>;
};

/// Type-erased callable/object holder with pluggable ownership policy.
template <
    typename target_t,
    template <typename, template <typename> class> class ownership_template =
        fn::reference_counting,
    typename error_policy = fn::assert_on_error,
    std::size_t buffer_size = sizeof(void *),
    template <typename> class allocator_t = std::allocator>
class object_manager
    : private manager_helper<target_t, ownership_template, buffer_size,
                             allocator_t>,
      private manager_helper<target_t, ownership_template, buffer_size,
                             allocator_t>::allocator_type,
      public manager_helper<target_t, ownership_template, buffer_size,
                            allocator_t>::ownership_policy,
      public error_policy {
private:
  using helper =
      manager_helper<target_t, ownership_template, buffer_size, allocator_t>;

  using typename helper::allocator_type;
  using typename helper::buffer_type;
  using typename helper::ownership_policy;
  using typename helper::stored_type;

  /// Raw storage containing ownership-policy encoded payload.
  buffer_type local_buffer_;

  /// Returns internal allocator instance.
  [[nodiscard]] auto allocator() noexcept -> allocator_type & {
    return static_cast<allocator_type &>(*this);
  }

  /// Returns reference to encoded storage payload.
  [[nodiscard]] auto stored_object() const noexcept -> const stored_type & {
    return local_buffer_.template interpret_as<const stored_type &>();
  }

  /// Returns pointer to encoded storage payload.
  [[nodiscard]] auto stored_object_ptr() noexcept -> stored_type * {
    return static_cast<stored_type *>(local_buffer_.address());
  }

  /// Returns whether access checks are `noexcept`.
  [[nodiscard]] static consteval auto can_nothrow_access() noexcept -> bool {
    if constexpr (error_policy::check_before_access) {
      return noexcept(error_policy::on_access());
    }
    return true;
  }

public:
  /// Returns readonly access to the managed object.
  [[nodiscard]] auto access() const & noexcept(can_nothrow_access())
      -> const target_t & {
    if constexpr (error_policy::check_before_access) {
      if (is_invalid()) {
        error_policy::on_access();
        std::terminate();
      }
    }
    return ownership_policy::get_target(stored_object());
  }

  /// Returns mutable access to the managed object.
  [[nodiscard]] auto access() & noexcept(can_nothrow_access()) -> target_t & {
    return const_cast<target_t &>(
        static_cast<const object_manager &>(*this).access());
  }

  /// Returns moved readonly access to the managed object.
  [[nodiscard]] auto access() const && noexcept(can_nothrow_access())
      -> const target_t && {
    return static_cast<const target_t &&>(
        static_cast<const object_manager &>(*this).access());
  }

  /// Returns moved mutable access to the managed object.
  [[nodiscard]] auto access() && noexcept(can_nothrow_access()) -> target_t && {
    return const_cast<target_t &&>(
        static_cast<const object_manager &>(*this).access());
  }

  using helper::using_soo;

  template <typename... args_t>
    requires(
        !((std::is_same_v<object_manager, wh::core::remove_cvref_t<args_t>> &&
           ...) &&
          (sizeof...(args_t) == 1U)))
  explicit object_manager(args_t &&...args) noexcept(
      ownership_policy::template can_nothrow_construct<args_t...>)
    requires(helper::template is_constructible<args_t...>)
      : allocator_type(), ownership_policy(), error_policy() {
    static_assert(sizeof(typename helper::max_buffer) >= sizeof(buffer_type),
                  "Buffer size is too small for the specified target object "
                  "and the ownership policy!");

    ownership_policy::create(allocator(), stored_object_ptr(),
                             std::forward<args_t>(args)...);
  }

  ~object_manager() {
    if constexpr (error_policy::check_before_destroy) {
      if (is_invalid()) {
        error_policy::on_destroy();
        return;
      }
    }
    ownership_policy::destroy(allocator(), stored_object_ptr());
  }

  object_manager(const object_manager &other) noexcept(
      noexcept(error_policy::on_copy()) && ownership_policy::can_nothrow_copy)
    requires(helper::is_copy_constructible)
      : allocator_type(other), ownership_policy(other), error_policy(other) {
    if constexpr (error_policy::check_before_copy) {
      if (other.is_invalid()) {
        error_policy::on_copy();
        invalidate();
        return;
      }
    }
    ownership_policy::copy(allocator(), other.stored_object(),
                           stored_object_ptr());
  }

  object_manager(object_manager &&other) noexcept
      : allocator_type(std::move(other)), ownership_policy(std::move(other)),
        error_policy(std::move(other)),
        local_buffer_(std::move(other.local_buffer_)) {
    other.invalidate();
  }

  auto operator=(const object_manager &other) noexcept(
      noexcept(error_policy::on_copy()) && ownership_policy::can_nothrow_copy)
      -> object_manager & {
    object_manager{other}.swap(*this);
    return *this;
  }

  auto operator=(object_manager &&other) noexcept -> object_manager & {
    if constexpr (error_policy::check_before_destroy) {
      if (is_invalid()) {
        error_policy::on_destroy();
      } else {
        ownership_policy::destroy(allocator(), stored_object_ptr());
      }
    } else {
      ownership_policy::destroy(allocator(), stored_object_ptr());
    }

    allocator_type::operator=(std::move(other));
    local_buffer_ = std::move(other.local_buffer_);
    other.invalidate();

    return *this;
  }

  /// Swaps stored state with another object manager.
  template <typename other_t, typename other_error = std::decay_t<other_t>>
    requires(helper::is_copy_constructible)
  auto swap(other_t &&other) noexcept -> void {
    if constexpr (ownership_policy::allocator_traits::
                      propagate_on_container_swap::value ||
                  std::decay_t<other_t>::allocator_traits::
                      propagate_on_container_swap::value) {
      using std::swap;
      swap(allocator(), other.allocator());
    }

    buffer_type tmp_buffer{nullptr};
    tmp_buffer = std::move(other.local_buffer_);
    other.local_buffer_ = std::move(local_buffer_);
    local_buffer_ = std::move(tmp_buffer);
  }

  /// Marks storage as invalid without deallocating.
  auto invalidate() noexcept -> void {
    local_buffer_.head() = local_buffer_.address();
  }

  /// Returns whether storage is marked invalid.
  [[nodiscard]] auto is_invalid() const noexcept -> bool {
    return local_buffer_.head() == local_buffer_.address();
  }
};

} // namespace wh::core::fn_detail
