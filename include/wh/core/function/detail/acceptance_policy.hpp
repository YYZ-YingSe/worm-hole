// Defines callable acceptance rules used by function wrappers to decide
// whether a target type is storable for a given function signature.
#pragma once

#include <type_traits>

#include "wh/core/function/detail/utils.hpp"

namespace wh::core::fn_detail {

/// Shared callable-admission policy for function wrappers.
/// - `accept_non_copyables_v`: admits move-only functors.
/// - `accept_pointers_v`: admits pointer-like wrappers by dereferencing.
template <typename signature_t, bool accept_non_copyables_v,
          bool accept_pointers_v>
class acceptance_policy_base;

// Generates acceptance-policy specializations for each CV/ref-qualified
// signature.
#define WH_DEFINE_ACCEPTANCE_POLICY_BASE(CV, REF)                              \
  template <bool accept_non_copyables_v, bool accept_pointers_v,               \
            typename return_t, bool is_noexcept, typename... param_types>      \
  class acceptance_policy_base<return_t(param_types...)                        \
                                   CV REF noexcept(is_noexcept),               \
                               accept_non_copyables_v, accept_pointers_v> {    \
  private:                                                                     \
    template <typename fun_t>                                                  \
    [[nodiscard]] static consteval auto is_invocable_impl() -> bool {          \
      using decayed_t = std::decay_t<fun_t>;                                   \
      using cv_ref_t = decayed_t CV REF;                                       \
      if constexpr (is_noexcept) {                                             \
        return std::is_nothrow_invocable_r_v<return_t, cv_ref_t,               \
                                             param_types...>;                  \
      } else {                                                                 \
        return std::is_invocable_r_v<return_t, cv_ref_t, param_types...>;      \
      }                                                                        \
    }                                                                          \
    template <typename fun_t>                                                  \
    [[nodiscard]] static consteval auto is_eligible_impl() -> bool {           \
      using decayed_t = std::decay_t<fun_t>;                                   \
      if constexpr (is_invocable_impl<fun_t>()) {                              \
        if constexpr (!accept_non_copyables_v &&                               \
                      !std::is_copy_constructible_v<decayed_t>) {              \
          return false;                                                        \
        }                                                                      \
        return true;                                                           \
      }                                                                        \
      if constexpr (accept_pointers_v && is_dereferencable_v<decayed_t>) {     \
        using deref_t = dereferenced_t<decayed_t> CV REF;                      \
        if constexpr (is_noexcept) {                                           \
          return std::is_nothrow_invocable_r_v<return_t, deref_t,              \
                                               param_types...>;                \
        } else {                                                               \
          return std::is_invocable_r_v<return_t, deref_t, param_types...>;     \
        }                                                                      \
      }                                                                        \
      return false;                                                            \
    }                                                                          \
                                                                               \
  protected:                                                                   \
    static constexpr bool accept_non_copyables = accept_non_copyables_v;       \
    static constexpr bool accept_pointers = accept_pointers_v;                 \
    template <typename fun_t>                                                  \
    static constexpr bool is_eligible = is_eligible_impl<fun_t>();             \
    acceptance_policy_base() = default;                                        \
    ~acceptance_policy_base() = default;                                       \
  }

// Instantiate all CV/ref-qualified signature variants.
WH_DEFINE_ACCEPTANCE_POLICY_BASE(, );
WH_DEFINE_ACCEPTANCE_POLICY_BASE(const, );
WH_DEFINE_ACCEPTANCE_POLICY_BASE(, &);
WH_DEFINE_ACCEPTANCE_POLICY_BASE(, &&);
WH_DEFINE_ACCEPTANCE_POLICY_BASE(const, &);
WH_DEFINE_ACCEPTANCE_POLICY_BASE(const, &&);

#undef WH_DEFINE_ACCEPTANCE_POLICY_BASE

} // namespace wh::core::fn_detail

namespace wh::core::fn {

/// Accepts copyable callable values.
template <typename signature_t> struct standard_accept;

/// Accepts move-only callable values.
template <typename signature_t> struct non_copyable_accept;

/// Accepts callables and pointer-like wrappers.
template <typename signature_t> struct ptr_accept;

/// Accepts move-only callables and pointer-like wrappers.
template <typename signature_t> struct non_copyable_ptr_accept;

// Generates public acceptance-policy wrappers for each CV/ref-qualified
// signature.
#define WH_DEFINE_ACCEPTANCE_POLICIES(CV, REF)                                 \
  template <typename return_t, bool is_noexcept, typename... param_types>      \
  struct standard_accept<return_t(param_types...)                              \
                             CV REF noexcept(is_noexcept)>                     \
      : fn_detail::acceptance_policy_base<return_t(param_types...)             \
                                              CV REF noexcept(is_noexcept),    \
                                          false, false> {                      \
  protected:                                                                   \
    standard_accept() = default;                                               \
    ~standard_accept() = default;                                              \
  };                                                                           \
  template <typename return_t, bool is_noexcept, typename... param_types>      \
  struct non_copyable_accept<return_t(param_types...)                          \
                                 CV REF noexcept(is_noexcept)>                 \
      : fn_detail::acceptance_policy_base<return_t(param_types...)             \
                                              CV REF noexcept(is_noexcept),    \
                                          true, false> {                       \
  protected:                                                                   \
    non_copyable_accept() = default;                                           \
    ~non_copyable_accept() = default;                                          \
  };                                                                           \
  template <typename return_t, bool is_noexcept, typename... param_types>      \
  struct ptr_accept<return_t(param_types...) CV REF noexcept(is_noexcept)>     \
      : fn_detail::acceptance_policy_base<return_t(param_types...)             \
                                              CV REF noexcept(is_noexcept),    \
                                          false, true> {                       \
  protected:                                                                   \
    ptr_accept() = default;                                                    \
    ~ptr_accept() = default;                                                   \
  };                                                                           \
  template <typename return_t, bool is_noexcept, typename... param_types>      \
  struct non_copyable_ptr_accept<return_t(param_types...)                      \
                                     CV REF noexcept(is_noexcept)>             \
      : fn_detail::acceptance_policy_base<return_t(param_types...)             \
                                              CV REF noexcept(is_noexcept),    \
                                          true, true> {                        \
  protected:                                                                   \
    non_copyable_ptr_accept() = default;                                       \
    ~non_copyable_ptr_accept() = default;                                      \
  }

// Instantiate all CV/ref-qualified signature variants.
WH_DEFINE_ACCEPTANCE_POLICIES(, );
WH_DEFINE_ACCEPTANCE_POLICIES(const, );
WH_DEFINE_ACCEPTANCE_POLICIES(, &);
WH_DEFINE_ACCEPTANCE_POLICIES(, &&);
WH_DEFINE_ACCEPTANCE_POLICIES(const, &);
WH_DEFINE_ACCEPTANCE_POLICIES(const, &&);

#undef WH_DEFINE_ACCEPTANCE_POLICIES

} // namespace wh::core::fn
