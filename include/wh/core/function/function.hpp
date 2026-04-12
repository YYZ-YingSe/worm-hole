// Defines high-performance function wrapper types used across components.
#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

#include "wh/core/function/detail/acceptance_policy.hpp"
#include "wh/core/function/detail/error_policy.hpp"
#include "wh/core/function/detail/function_base.hpp"
#include "wh/core/function/detail/ownership_policy.hpp"
#include "wh/core/function/detail/storage_policy.hpp"

namespace wh::core {

// Re-export policy/alias types into `wh::core` for ergonomics.
using fn::assert_on_error;
using fn::check_none;
using fn::deep_copy;
using fn::non_copyable_accept;
using fn::non_copyable_ptr_accept;
using fn::owning_storage;
using fn::ptr_accept;
using fn::ref_only_storage;
using fn::reference_counting;
using fn::skip_on_error;
using fn::standard_accept;
using fn::throw_on_error;

/// Generic function wrapper with configurable storage/ownership/error policies.
template <
    typename signature_t,
    template <typename, template <typename, template <typename> class> class,
              template <typename> class, class, std::size_t,
              template <typename> class> class storage_policy_t =
        owning_storage,
    template <typename, template <typename> class> class ownership_policy_t =
        reference_counting,
    template <typename> class acceptance_policy_t = standard_accept,
    typename error_policy_t = assert_on_error,
    std::size_t buffer_size_v = sizeof(void *),
    template <typename> class allocator_t = std::allocator>
class function;

template <typename signature_t>
/// Move-only function wrapper alias.
using move_only_function =
    function<signature_t, owning_storage, deep_copy, non_copyable_accept>;

template <typename signature_t>
/// Non-owning callable reference wrapper alias.
using function_ref = function<signature_t, ref_only_storage,
                              fn_detail::local_ownership, standard_accept>;

template <typename signature_t>
/// `std::function`-like wrapper alias (throw-on-error policy).
using standard_function =
    function<signature_t, owning_storage, deep_copy, standard_accept,
             throw_on_error, fn_detail::member_pointer_size>;

template <typename signature_t>
/// Callback-oriented callable wrapper alias.
using callback_function =
    function<signature_t, owning_storage, deep_copy, standard_accept,
             skip_on_error, fn_detail::member_pointer_size>;

template <typename>
/// Trait: false by default for non-function-wrapper types.
inline constexpr bool is_function_v = false;

/// Trait specialization: true for configured `wh::core::function` wrappers.
template <
    typename signature_t,
    template <typename, template <typename, template <typename> class> class,
              template <typename> class, class, std::size_t,
              template <typename> class> class storage_policy_t,
    template <typename, template <typename> class> class ownership_policy_t,
    template <typename> class acceptance_policy_t, typename error_policy_t,
    std::size_t buffer_size_v, template <typename> class allocator_t>
inline constexpr bool is_function_v<
    function<signature_t, storage_policy_t, ownership_policy_t,
             acceptance_policy_t, error_policy_t, buffer_size_v, allocator_t>> =
    true;

template <typename type_t>
/// Type-trait wrapper around `is_function_v`.
using is_function = std::bool_constant<is_function_v<type_t>>;

// Generates `function<>` specializations for each CV/ref-qualified signature.
#define WH_DEFINE_FUNCTION(CV, REF, INV_QUALS)                                 \
  template <                                                                   \
      template <typename,                                                      \
                template <typename, template <typename> class> class,          \
                template <typename> class, class, std::size_t,                 \
                template <typename> class> class storage_policy_t,             \
      template <typename, template <typename> class> class ownership_policy_t, \
      template <typename> class acceptance_policy_t, typename error_policy_t,  \
      std::size_t buffer_size_v, template <typename> class allocator_t,        \
      typename return_t, bool is_noexcept, typename... param_types>            \
  class function<return_t(param_types...) CV REF noexcept(is_noexcept),        \
                 storage_policy_t, ownership_policy_t, acceptance_policy_t,    \
                 error_policy_t, buffer_size_v, allocator_t>                   \
      : private fn_detail::function_base<storage_policy_t<                     \
            return_t(param_types...) CV REF noexcept(is_noexcept),             \
            ownership_policy_t, acceptance_policy_t, error_policy_t,           \
            buffer_size_v, allocator_t>> {                                     \
  private:                                                                     \
    using signature_type = return_t(param_types...) CV REF                     \
        noexcept(is_noexcept);                                                 \
    using base = fn_detail::function_base<storage_policy_t<                    \
        signature_type, ownership_policy_t, acceptance_policy_t,               \
        error_policy_t, buffer_size_v, allocator_t>>;                          \
    using storage = typename base::storage_type;                               \
    using base::local_buffer_;                                                 \
                                                                               \
  public:                                                                      \
    function() = delete;                                                       \
    ~function() = default;                                                     \
    function(const function &other) = default;                                 \
    function(function &&other) noexcept = default;                             \
    auto operator=(const function &) -> function & = default;                  \
    auto operator=(function &&) noexcept -> function & = default;              \
    function(std::nullptr_t) noexcept : base(nullptr) {}                       \
    auto operator=(std::nullptr_t) noexcept -> function & {                    \
      base::operator=(nullptr);                                                \
      return *this;                                                            \
    }                                                                          \
    template <typename fun_t>                                                  \
    static constexpr bool using_soo =                                          \
        storage::template traits<fun_t>::using_soo;                            \
    template <typename fun_t, typename decayed_fun_t = std::decay_t<fun_t>>    \
      requires((!std::is_same_v<decayed_fun_t, function>) &&                   \
               (!fn_detail::is_in_place_type_v<decayed_fun_t>) &&              \
               (storage::template can_create_from<fun_t, fun_t>()))            \
    function(fun_t &&fun) noexcept(                                            \
        base::template check_nothrow<decayed_fun_t, fun_t>())                  \
        : base(std::in_place_type<fun_t>, std::forward<fun_t>(fun)) {          \
      static_assert(storage::template is_invocable_v<decayed_fun_t>);          \
    }                                                                          \
    template <typename fun_t, typename... args_t>                              \
      requires(fn_detail::is_direct_constructible_v<fun_t, args_t...> &&       \
               storage::template can_create_from<fun_t, args_t...>())          \
    explicit function(std::in_place_type_t<fun_t>, args_t &&...args) noexcept( \
        base::template check_nothrow<fun_t, args_t...>())                      \
        : base(std::in_place_type<fun_t>, std::forward<args_t>(args)...) {     \
      static_assert(std::is_same_v<std::decay_t<fun_t>, fun_t>);               \
      static_assert(storage::template is_invocable_v<std::decay_t<fun_t>>);    \
    }                                                                          \
    template <typename fun_t, typename type_t, typename... args_t>             \
      requires(fn_detail::is_direct_constructible_v<                           \
                   fun_t, std::initializer_list<type_t> &, args_t...> &&       \
               storage::template can_create_from<                              \
                   fun_t, std::initializer_list<type_t> &, args_t...>())       \
    explicit function(                                                         \
        std::in_place_type_t<fun_t>, std::initializer_list<type_t> init_list,  \
        args_t &&...args) noexcept(base::                                      \
                                       template check_nothrow<                 \
                                           fun_t,                              \
                                           std::initializer_list<type_t> &,    \
                                           args_t...>())                       \
        : base(std::in_place_type<fun_t>, init_list,                           \
               std::forward<args_t>(args)...) {                                \
      static_assert(std::is_same_v<std::decay_t<fun_t>, fun_t>);               \
      static_assert(storage::template is_invocable_v<std::decay_t<fun_t>>);    \
    }                                                                          \
    template <typename fun_t, typename decayed_fun_t = std::decay_t<fun_t>>    \
      requires std::is_constructible_v<function, fun_t> &&                     \
               (!std::is_same_v<decayed_fun_t, function>) &&                   \
               (storage::template is_invocable_v<decayed_fun_t>)               \
    auto operator=(fun_t &&fun) noexcept(                                      \
        base::template check_nothrow<decayed_fun_t, fun_t>()) -> function & {  \
      function{std::forward<fun_t>(fun)}.swap(*this);                          \
      return *this;                                                            \
    }                                                                          \
    [[nodiscard]] explicit operator bool() const noexcept {                    \
      return !storage::is_empty(local_buffer_);                                \
    }                                                                          \
    auto operator()(param_types... params) CV REF noexcept(is_noexcept)        \
        -> return_t {                                                          \
      if constexpr (storage::error_handler::check_before_call) {               \
        if (storage::is_empty(local_buffer_)) {                                \
          storage::error_handler::on_invoke();                                 \
        }                                                                      \
      }                                                                        \
      using buffer_inv_quals = typename storage::buffer_type INV_QUALS;        \
      return std::invoke(                                                      \
          storage::access(std::forward<buffer_inv_quals>(local_buffer_)),      \
          std::forward<param_types>(params)...);                               \
    }                                                                          \
    auto swap(function &other) noexcept -> void { base::swap(other); }         \
    friend auto swap(function &lhs, function &rhs) noexcept -> void {          \
      lhs.swap(rhs);                                                           \
    }                                                                          \
    friend auto operator==(const function &lhs, std::nullptr_t) noexcept       \
        -> bool {                                                              \
      return storage::is_empty(lhs.local_buffer_);                             \
    }                                                                          \
  }

// Instantiate all CV/ref-qualified signature variants.
// (no CV, no REF) -> &
// (const, no REF) -> const &
// (no CV, &) -> &
// (no CV, &&) -> &&
// (const, &) -> const &
// (const, &&) -> const &&
WH_DEFINE_FUNCTION(, , &);
WH_DEFINE_FUNCTION(const, , const &);
WH_DEFINE_FUNCTION(, &, &);
WH_DEFINE_FUNCTION(, &&, &&);
WH_DEFINE_FUNCTION(const, &, const &);
WH_DEFINE_FUNCTION(const, &&, const &&);

#undef WH_DEFINE_FUNCTION

} // namespace wh::core
