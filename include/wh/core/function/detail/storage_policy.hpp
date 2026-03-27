// Defines storage policy selection logic for function wrappers, choosing
// inline vs heap layout based on callable size and traits.
#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <type_traits>

#include "wh/core/function/detail/storage.hpp"
#include "wh/core/function/detail/utils.hpp"
#include "wh/core/function/detail/vtable.hpp"

namespace wh::core::fn_detail {

/// Traits describing whether a handler can be constructed in a storage buffer.
template <typename buffer_t, typename handler_t> struct storage_traits {
  template <typename... args_t>
  static constexpr bool is_constructible =
      buffer_t::template can_construct<handler_t>() &&
      std::is_constructible_v<handler_t, args_t...>;

  template <typename... args_t>
  static constexpr bool is_nothrow_constructible =
      is_constructible<args_t...> &&
      std::is_nothrow_constructible_v<handler_t, args_t...>;
};

/// Storage base that manages callable lifetime.
template <typename signature_t, template <typename> class acceptance_policy,
          typename error_policy, std::size_t buffer_size>
class managing_storage_base : public acceptance_policy<signature_t>,
                              public error_policy {
protected:
  using error_handler = error_policy;
  using acceptance = acceptance_policy<signature_t>;
  using vtable_type = std::conditional_t<acceptance::accept_non_copyables,
                                         destructible_vtable<signature_t>,
                                         cloneable_vtable<signature_t>>;

  static constexpr std::size_t size_value =
      add_padding_to_size<alignof(void *)>(sizeof(vtable_type)) +
      std::max<std::size_t>(buffer_size, sizeof(void *));

  static constexpr std::size_t alignment_value =
      std::max<std::size_t>(alignof(vtable_type), alignof(void *));

  static constexpr std::size_t padded_size =
      add_padding_to_size<alignment_value>(size_value);

  using buffer_type = any_storage<padded_size>;

  /// Returns whether access checks are `noexcept`.
  [[nodiscard]] static consteval auto can_nothrow_access() -> bool {
    if constexpr (error_handler::check_before_access) {
      return noexcept(error_handler::on_access());
    }
    return true;
  }

  [[nodiscard]] static auto
  access(const buffer_type &local_buffer) noexcept(can_nothrow_access())
      -> const vtable_type & {
    if constexpr (error_handler::check_before_access) {
      if (is_empty(local_buffer)) {
        error_handler::on_access();
        std::terminate();
      }
    }
    return local_buffer.template interpret_as<const vtable_type &>();
  }

  [[nodiscard]] static auto
  access(buffer_type &local_buffer) noexcept(can_nothrow_access())
      -> vtable_type & {
    return const_cast<vtable_type &>(
        access(static_cast<const buffer_type &>(local_buffer)));
  }

  [[nodiscard]] static auto
  access(const buffer_type &&local_buffer) noexcept(can_nothrow_access())
      -> const vtable_type && {
    return static_cast<const vtable_type &&>(
        access(static_cast<const buffer_type &>(local_buffer)));
  }

  [[nodiscard]] static auto
  access(buffer_type &&local_buffer) noexcept(can_nothrow_access())
      -> vtable_type && {
    return const_cast<vtable_type &&>(
        access(static_cast<const buffer_type &>(local_buffer)));
  }

  static auto copy(const buffer_type &source, buffer_type &destination) -> void
    requires(!acceptance::accept_non_copyables)
  {
    if constexpr (error_handler::check_before_copy) {
      if (is_empty(source)) {
        error_handler::on_copy();
        set_to_empty(destination);
        return;
      }
    }
    (access(source)).clone_itself(destination.address());
  }

  static auto destroy(const buffer_type &target) noexcept -> void {
    if constexpr (error_handler::check_before_destroy) {
      if (is_empty(target)) {
        error_handler::on_destroy();
        return;
      }
    }
    access(target).~vtable_type();
  }

  /// Returns whether storage currently holds no callable.
  [[nodiscard]] static auto is_empty(const buffer_type &local_buffer) noexcept
      -> bool {
    return local_buffer.head() == nullptr;
  }

  /// Clears storage to the canonical empty sentinel state.
  static auto set_to_empty(buffer_type &local_buffer) noexcept -> void {
    local_buffer = nullptr;
  }

  managing_storage_base() = default;
  ~managing_storage_base() = default;
};

/// Storage base that does not own callable lifetime.
template <typename signature_t, template <typename> class acceptance_policy,
          typename error_policy, std::size_t buffer_size>
class non_managing_storage_base : public acceptance_policy<signature_t>,
                                  public error_policy {
protected:
  using error_handler = error_policy;
  using acceptance = acceptance_policy<signature_t>;
  using vtable_type = simple_vtable<signature_t>;

  static constexpr std::size_t size_value = sizeof(vtable_type) + buffer_size;
  static constexpr std::size_t alignment_value = alignof(vtable_type);

  static constexpr std::size_t storage_size =
      add_padding_to_size<alignment_value>(size_value);

  using buffer_type = any_storage<storage_size>;

  /// Returns whether access checks are `noexcept`.
  [[nodiscard]] static consteval auto can_nothrow_access() -> bool {
    if constexpr (error_handler::check_before_access) {
      return noexcept(error_handler::on_access());
    }
    return true;
  }

  [[nodiscard]] static auto
  access(const buffer_type &local_buffer) noexcept(can_nothrow_access())
      -> const vtable_type & {
    if constexpr (error_handler::check_before_access) {
      if (is_empty(local_buffer)) {
        error_handler::on_access();
        std::terminate();
      }
    }
    return local_buffer.template interpret_as<const vtable_type &>();
  }

  [[nodiscard]] static auto
  access(buffer_type &local_buffer) noexcept(can_nothrow_access())
      -> vtable_type & {
    return const_cast<vtable_type &>(
        access(static_cast<const buffer_type &>(local_buffer)));
  }

  [[nodiscard]] static auto
  access(const buffer_type &&local_buffer) noexcept(can_nothrow_access())
      -> const vtable_type && {
    return static_cast<const vtable_type &&>(
        access(static_cast<const buffer_type &>(local_buffer)));
  }

  [[nodiscard]] static auto
  access(buffer_type &&local_buffer) noexcept(can_nothrow_access())
      -> vtable_type && {
    return const_cast<vtable_type &&>(
        access(static_cast<const buffer_type &>(local_buffer)));
  }

  static auto
  copy(const buffer_type &source,
       buffer_type &destination) noexcept(noexcept(error_handler::on_copy()))
      -> void
    requires(!acceptance::accept_non_copyables)
  {
    if constexpr (error_handler::check_before_copy) {
      if (is_empty(source)) {
        error_handler::on_copy();
        set_to_empty(destination);
        return;
      }
    }
    destination = source;
  }

  static auto destroy(const buffer_type &target) noexcept -> void {
    if constexpr (error_handler::check_before_destroy) {
      if (is_empty(target)) {
        error_handler::on_destroy();
      }
    }
  }

  /// Returns whether storage currently holds no callable.
  [[nodiscard]] static auto is_empty(const buffer_type &local_buffer) noexcept
      -> bool {
    return local_buffer.head() == nullptr;
  }

  /// Clears storage to the canonical empty sentinel state.
  static auto set_to_empty(buffer_type &local_buffer) noexcept -> void {
    local_buffer = nullptr;
  }

  non_managing_storage_base() = default;
  ~non_managing_storage_base() = default;
};

} // namespace wh::core::fn_detail

namespace wh::core::fn {

/// Owning callable storage policy.
template <typename signature_t,
          template <typename, template <typename> class> class ownership_policy,
          template <typename> class acceptance_policy, typename error_policy,
          std::size_t buffer_size, template <typename> class allocator_t>
class owning_storage
    : protected fn_detail::managing_storage_base<signature_t, acceptance_policy,
                                                 error_policy, buffer_size> {
private:
  using parent =
      fn_detail::managing_storage_base<signature_t, acceptance_policy,
                                       error_policy, buffer_size>;

  using typename parent::acceptance;
  using typename parent::vtable_type;

  template <typename fun_t>
  using handler =
      fn_detail::vtable_handler<signature_t, std::decay_t<fun_t>, vtable_type,
                                ownership_policy, buffer_size,
                                acceptance::accept_pointers, allocator_t>;

protected:
  using parent::access;
  using parent::copy;
  using parent::destroy;
  using parent::is_empty;
  using parent::set_to_empty;
  using typename parent::buffer_type;
  using typename parent::error_handler;

  /// Type traits for callable storage characteristics.
  template <typename fun_t>
  struct traits
      : public fn_detail::storage_traits<buffer_type, handler<fun_t>> {
    /// Whether callable can use small-object optimization.
    static constexpr bool using_soo = handler<fun_t>::using_soo;
    /// Owning storage never stores plain references.
    static constexpr bool storing_reference = false;
  };

  template <typename fun_t>
  static constexpr bool is_invocable_v =
      acceptance::template is_eligible<fun_t>;

  template <typename fun_t, typename... args_t>
    requires(acceptance::template is_eligible<fun_t>)
  static auto create(buffer_type &target, args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<handler<fun_t>, args_t...>) -> void {
    ::new (target.address()) handler<fun_t>(std::forward<args_t>(args)...);

    static_assert(traits<fun_t>::template is_constructible<args_t...>,
                  "Storage is not constructible!");
  }

  owning_storage() = default;
  ~owning_storage() = default;
};

/// Non-owning callable reference storage policy.
template <typename signature_t,
          template <typename, template <typename> class> class ownership_policy,
          template <typename> class acceptance_policy, typename error_policy,
          std::size_t buffer_size, template <typename> class allocator_t>
  requires is_same_ownership_policy_v<ownership_policy,
                                      fn_detail::local_ownership>
class ref_only_storage
    : protected fn_detail::non_managing_storage_base<
          signature_t, acceptance_policy, error_policy, buffer_size> {
private:
  using parent =
      fn_detail::non_managing_storage_base<signature_t, acceptance_policy,
                                           error_policy, buffer_size>;

  using typename parent::acceptance;
  using typename parent::vtable_type;

  template <typename fun_t>
  using handler =
      fn_detail::vtable_handler<signature_t, std::remove_reference_t<fun_t> *,
                                vtable_type, fn_detail::local_ownership,
                                buffer_size, true, allocator_t>;

protected:
  using parent::access;
  using parent::copy;
  using parent::destroy;
  using parent::is_empty;
  using parent::set_to_empty;
  using typename parent::buffer_type;
  using typename parent::error_handler;

  /// Type traits for callable storage characteristics.
  template <typename fun_t>
  struct traits
      : public fn_detail::storage_traits<buffer_type, handler<fun_t>> {
    /// Reference-only storage is always in-place.
    static constexpr bool using_soo = true;
    /// Reference-only storage stores external object references.
    static constexpr bool storing_reference = true;
  };

  template <typename fun_t>
  static constexpr bool is_invocable_v =
      acceptance::template is_eligible<fun_t>;

  template <typename fun_t>
    requires(acceptance::template is_eligible<fun_t>)
  static auto create(buffer_type &target, fun_t &&invocable) noexcept -> void
    requires(traits<fun_t>::template is_constructible<
             std::remove_reference_t<fun_t> *>)
  {
    ::new (target.address()) handler<fun_t>(std::addressof(invocable));

    static_assert(traits<fun_t>::template is_constructible<
                      std::remove_reference_t<fun_t> *>,
                  "Storage is not constructible!");
  }

  ref_only_storage() = default;
  ~ref_only_storage() = default;
};

/// Type trait for comparing storage policy templates.
template <
    template <typename, template <typename, template <typename> class> class,
              template <typename> class, class, std::size_t,
              template <typename> class> class,
    template <typename, template <typename, template <typename> class> class,
              template <typename> class, class, std::size_t,
              template <typename> class> class>
struct is_same_storage_policy : std::false_type {};

/// Type trait specialization for identical storage policies.
template <
    template <typename, template <typename, template <typename> class> class,
              template <typename> class, class, std::size_t,
              template <typename> class> class policy_t>
struct is_same_storage_policy<policy_t, policy_t> : std::true_type {};

template <
    template <typename, template <typename, template <typename> class> class,
              template <typename> class, class, std::size_t,
              template <typename> class> class policy1_t,
    template <typename, template <typename, template <typename> class> class,
              template <typename> class, class, std::size_t,
              template <typename> class> class policy2_t>
inline constexpr bool is_same_storage_policy_v =
    is_same_storage_policy<policy1_t, policy2_t>::value;

} // namespace wh::core::fn
