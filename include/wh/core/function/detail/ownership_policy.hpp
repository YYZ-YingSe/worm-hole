// Defines ownership policies for function wrappers, controlling whether
// call targets are owning, borrowing, or non-copying references.
#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

#include "wh/core/function/detail/storage.hpp"
#include "wh/core/function/detail/utils.hpp"

namespace wh::core::fn_detail {

/// Intrusive reference-counted object holder.
template <typename target_t, template <typename> class allocator_t>
class reference_counting_wrapper {
public:
  using allocator_type = allocator_t<reference_counting_wrapper>;
  using allocator_traits = std::allocator_traits<allocator_type>;

private:
  /// Intrusive reference count shared by wrapper instances.
  std::atomic<int> count_;
  /// Wrapped target object.
  target_t obj_;

public:
  /// Allocates and constructs a wrapped target instance.
  template <typename... args_t>
  [[nodiscard]] static auto create_new(allocator_type &alloc, args_t &&...args)
      -> reference_counting_wrapper * {
    reference_counting_wrapper *ptr = allocator_traits::allocate(alloc, 1U);
    scope_guard allocation_guard{
        [&alloc, &ptr]() { allocator_traits::deallocate(alloc, ptr, 1U); }};

    allocator_traits::construct(alloc, ptr, std::forward<args_t>(args)...);
    allocation_guard.disarm();

    return ptr;
  }

  /// Returns mutable access to the wrapped object.
  [[nodiscard]] static auto access(reference_counting_wrapper *target) noexcept -> target_t & {
    return target->obj_;
  }

  /// Increments the reference count and returns the same wrapper.
  [[nodiscard]] static auto copy(reference_counting_wrapper *target) noexcept
      -> reference_counting_wrapper * {
    (target->count_).fetch_add(1, std::memory_order_relaxed);
    return target;
  }

  static auto destroy(allocator_type &alloc, reference_counting_wrapper *target) noexcept -> void {
    int count = (target->count_).fetch_sub(1, std::memory_order_release) - 1;
    if (count == 0) {
      std::atomic_thread_fence(std::memory_order_acquire);
      allocator_traits::destroy(alloc, target);
      allocator_traits::deallocate(alloc, target, 1U);
    }
  }

  template <typename... args_t>
  explicit reference_counting_wrapper(args_t &&...args)
      : count_(1), obj_(std::forward<args_t>(args)...) {}

  ~reference_counting_wrapper() = default;
};

/// In-buffer ownership policy for small callable objects.
template <typename target_t, template <typename> class allocator_t>
  requires std::same_as<target_t, std::decay_t<target_t>>
class local_ownership {
public:
  using allocator_type = allocator_t<target_t>;
  using allocator_traits = std::allocator_traits<allocator_type>;
  using stored_type = std::decay_t<target_t>;

  template <typename... args_t>
  static constexpr bool can_nothrow_construct =
      fn_detail::is_nothrow_direct_constructible_v<stored_type, args_t...>;

  static constexpr bool can_nothrow_copy = can_nothrow_construct<const stored_type &>;

protected:
  local_ownership() = default;
  ~local_ownership() = default;

  /// Returns a stable reference to the stored target.
  [[nodiscard]] static auto get_target(const stored_type &stored) noexcept -> const target_t & {
    return stored;
  }

  template <typename... args_t>
  static auto create(allocator_type &alloc, stored_type *target,
                     args_t &&...args) noexcept(can_nothrow_construct<args_t...>) -> void {
    allocator_traits::construct(alloc, target, std::forward<args_t>(args)...);
  }

  static auto copy(allocator_type &alloc, const stored_type &source,
                   stored_type *destination) noexcept(can_nothrow_copy) -> void {
    allocator_traits::construct(alloc, destination, source);
  }

  static auto destroy(allocator_type &alloc, stored_type *target) noexcept -> void {
    allocator_traits::destroy(alloc, target);
  }
};

} // namespace wh::core::fn_detail

namespace wh::core::fn {

/// Deep-copy ownership policy.
template <typename target_t, template <typename> class allocator_t>
  requires std::same_as<target_t, std::decay_t<target_t>>
class deep_copy;

/// Shared ownership policy backed by intrusive refcounting.
template <typename target_t, template <typename> class allocator_t>
  requires std::same_as<target_t, std::decay_t<target_t>>
class reference_counting;

/// Type trait for comparing ownership policy templates.
template <template <typename, template <typename> class> class,
          template <typename, template <typename> class> class>
struct is_same_ownership_policy : std::false_type {};

/// Type trait specialization for identical ownership policies.
template <template <typename, template <typename> class> class policy_t>
struct is_same_ownership_policy<policy_t, policy_t> : std::true_type {};

template <template <typename, template <typename> class> class policy1_t,
          template <typename, template <typename> class> class policy2_t>
inline constexpr bool is_same_ownership_policy_v =
    is_same_ownership_policy<policy1_t, policy2_t>::value;

/// Heap-owning deep-copy policy implementation.
template <typename target_t, template <typename> class allocator_t>
  requires std::same_as<target_t, std::decay_t<target_t>>
class deep_copy {
public:
  using allocator_type = allocator_t<target_t>;
  using allocator_traits = std::allocator_traits<allocator_type>;
  using stored_type = target_t *;

  template <typename... args_t> static constexpr bool can_nothrow_construct = false;

  static constexpr bool can_nothrow_copy = false;

protected:
  deep_copy() = default;
  ~deep_copy() = default;

  /// Returns a stable reference to the owned target.
  [[nodiscard]] static auto get_target(const stored_type &stored) noexcept -> const target_t & {
    return *stored;
  }

  template <typename... args_t>
  static auto create(allocator_type &alloc, stored_type *target, args_t &&...args) -> void {
    static_assert(fn_detail::is_direct_constructible_v<std::decay_t<target_t>, args_t...>,
                  "Target object must be constructible");

    *target = allocator_traits::allocate(alloc, 1U);
    fn_detail::scope_guard allocation_guard{
        [&alloc, &target]() { allocator_traits::deallocate(alloc, *target, 1U); }};

    allocator_traits::construct(alloc, *target, std::forward<args_t>(args)...);
    allocation_guard.disarm();
  }

  static auto copy(allocator_type &alloc, const stored_type &source, stored_type *destination)
      -> void {
    static_assert(std::is_copy_constructible_v<std::decay_t<target_t>>,
                  "Target object must be copy-constructible");

    create(alloc, destination, *source);
  }

  static auto destroy(allocator_type &alloc, stored_type *target) noexcept -> void {
    allocator_traits::destroy(alloc, *target);
    allocator_traits::deallocate(alloc, *target, 1U);
  }

  deep_copy(const reference_counting<target_t, allocator_t> &) = delete;
};

/// Heap-owning shared policy implementation.
template <typename target_t, template <typename> class allocator_t>
  requires std::same_as<target_t, std::decay_t<target_t>>
class reference_counting {
private:
  using wrapped_target = fn_detail::reference_counting_wrapper<target_t, allocator_t>;

public:
  using allocator_type = typename wrapped_target::allocator_type;
  using allocator_traits = std::allocator_traits<allocator_type>;
  using stored_type = wrapped_target *;

  template <typename... args_t> static constexpr bool can_nothrow_construct = false;

  static constexpr bool can_nothrow_copy = false;

protected:
  reference_counting() = default;
  ~reference_counting() = default;

  /// Returns a stable reference to the shared target.
  [[nodiscard]] static auto get_target(const stored_type &stored) noexcept -> const target_t & {
    return wrapped_target::access(stored);
  }

  template <typename... args_t>
  static auto create(allocator_type &alloc, stored_type *target, args_t &&...args) -> void {
    static_assert(fn_detail::is_direct_constructible_v<std::decay_t<target_t>, args_t...>,
                  "Target object must be constructible");

    *target = wrapped_target::create_new(alloc, std::forward<args_t>(args)...);
  }

  static auto copy(allocator_type &, const stored_type &source, stored_type *destination) noexcept
      -> void {
    *destination = wrapped_target::copy(source);
  }

  static auto destroy(allocator_type &alloc, stored_type *target) noexcept -> void {
    wrapped_target::destroy(alloc, *target);
  }

  reference_counting(const deep_copy<target_t, allocator_t> &) {}
};

} // namespace wh::core::fn
