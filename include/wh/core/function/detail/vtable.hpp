// Defines vtable-style dispatch entries for type-erased function wrappers,
// including invoke, move, copy, and destroy operations.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "wh/core/function/detail/object_manager.hpp"
#include "wh/core/function/detail/utils.hpp"

namespace wh::core::fn_detail {

/// Base vtable with invocation only.
template <typename signature_t> class simple_vtable;

/// Base vtable with virtual destruction.
template <typename signature_t> class destructible_vtable;

/// Base vtable with clone support.
template <typename signature_t> class cloneable_vtable;

/// Concrete vtable adapter around a managed invocable.
template <typename signature_t, typename invocable_t, typename vtable_t,
          template <typename, template <typename> class> class ownership_policy,
          std::size_t buffer_size, bool accept_pointers,
          template <typename> class allocator_t>
class vtable_handler;

// Generates pure-invoke vtable specializations per CV/ref-qualified signature.
#define WH_DEFINE_SIMPLE_VTABLE(CV, REF)                                       \
  template <typename return_t, bool is_noexcept, typename... param_types>      \
  class simple_vtable<return_t(param_types...) CV REF noexcept(is_noexcept)> { \
  public:                                                                      \
    virtual auto operator()(ref_non_trivials<param_types>...) CV REF           \
        noexcept(is_noexcept) -> return_t = 0;                                 \
                                                                               \
  protected:                                                                   \
    ~simple_vtable() = default;                                                \
  }

// Generates invoke+virtual-dtor vtable specializations.
#define WH_DEFINE_DESTRUCTIBLE_VTABLE(CV, REF)                                 \
  template <typename return_t, bool is_noexcept, typename... param_types>      \
  class destructible_vtable<return_t(param_types...)                           \
                                CV REF noexcept(is_noexcept)> {                \
  public:                                                                      \
    virtual auto operator()(ref_non_trivials<param_types>...) CV REF           \
        noexcept(is_noexcept) -> return_t = 0;                                 \
    virtual ~destructible_vtable() = default;                                  \
  }

// Generates invoke+clone vtable specializations.
#define WH_DEFINE_CLONEABLE_VTABLE(CV, REF)                                    \
  template <typename return_t, bool is_noexcept, typename... param_types>      \
  class cloneable_vtable<return_t(param_types...)                              \
                             CV REF noexcept(is_noexcept)> {                   \
  public:                                                                      \
    virtual auto operator()(ref_non_trivials<param_types>...) CV REF           \
        noexcept(is_noexcept) -> return_t = 0;                                 \
    virtual auto clone_itself(void *destination) const -> void = 0;            \
    virtual ~cloneable_vtable() = default;                                     \
  }

// Generates concrete vtable handlers bridging invocation to object_manager.
#define WH_DEFINE_VTABLE_HANDLER(CV, REF, INV_QUALS)                           \
  template <                                                                   \
      typename invocable_t, typename vtable_t,                                 \
      template <typename, template <typename> class> class ownership_policy,   \
      std::size_t buffer_size, bool accept_pointers,                           \
      template <typename> class allocator_t, typename return_t,                \
      bool is_noexcept, typename... param_types>                               \
  class vtable_handler<return_t(param_types...) CV REF noexcept(is_noexcept),  \
                       invocable_t, vtable_t, ownership_policy, buffer_size,   \
                       accept_pointers, allocator_t>                           \
      final : public vtable_t {                                                \
  private:                                                                     \
    using manager_t =                                                          \
        object_manager<invocable_t, ownership_policy, check_none_base,         \
                       buffer_size, allocator_t>;                              \
    manager_t manager_;                                                        \
    template <typename type_t>                                                 \
    [[nodiscard]] static consteval auto basic_constraint() -> bool {           \
      return is_direct_constructible_v<std::decay_t<invocable_t>, type_t> &&   \
             (!std::is_same_v<vtable_handler,                                  \
                              wh::core::remove_cvref_t<type_t>>);              \
    }                                                                          \
    template <typename... args_t>                                              \
      requires(sizeof...(args_t) > 1U)                                         \
    [[nodiscard]] static consteval auto basic_constraint() -> bool {           \
      return is_direct_constructible_v<std::decay_t<invocable_t>, args_t...>;  \
    }                                                                          \
                                                                               \
  public:                                                                      \
    static constexpr bool using_soo = manager_t::using_soo;                    \
    template <typename... args_t>                                              \
    static constexpr bool is_constructible =                                   \
        basic_constraint<args_t...>() &&                                       \
        manager_t::template is_constructible<args_t...>;                       \
    template <typename... args_t>                                              \
    static constexpr bool is_nothrow_constructible =                           \
        is_constructible<args_t...> &&                                         \
        manager_t::template is_nothrow_constructible<args_t...>;               \
    template <typename... args_t>                                              \
      requires(is_constructible<args_t...>)                                    \
    explicit vtable_handler(args_t &&...args) noexcept(                        \
        is_nothrow_constructible<args_t...>)                                   \
        : manager_(std::forward<args_t>(args)...) {}                           \
    vtable_handler(const vtable_handler &other) = default;                     \
    vtable_handler(vtable_handler &&other) noexcept = delete;                  \
    auto operator=(const vtable_handler &) -> vtable_handler & = delete;       \
    auto operator=(vtable_handler &&) noexcept -> vtable_handler & = delete;   \
    ~vtable_handler() = default;                                               \
    auto clone_itself(void *destination) const -> void {                       \
      ::new (destination) vtable_handler(*this);                               \
    }                                                                          \
    auto operator()(ref_non_trivials<param_types>... params) CV REF            \
        noexcept(is_noexcept) -> return_t final {                              \
      using fun_inv_quals = invocable_t INV_QUALS;                             \
      if constexpr (is_invocable<fun_inv_quals, return_t, is_noexcept,         \
                                 param_types...>::value) {                     \
        return std::invoke(                                                    \
            std::forward<fun_inv_quals>(manager_.access()),                    \
            std::forward<ref_non_trivials<param_types>>(params)...);           \
      } else if constexpr (accept_pointers &&                                  \
                           is_dereferencable_v<invocable_t>) {                 \
        using deref_inv_quals = dereferenced_t<invocable_t> INV_QUALS;         \
        if constexpr (is_invocable<deref_inv_quals, return_t, is_noexcept,     \
                                   param_types...>::value) {                   \
          return std::invoke(                                                  \
              std::forward<deref_inv_quals>(*manager_.access()),               \
              std::forward<ref_non_trivials<param_types>>(params)...);         \
        } else {                                                               \
          static_assert(make_false<invocable_t>(),                             \
                        "Target does not point to an invocable object!");      \
        }                                                                      \
      } else {                                                                 \
        static_assert(make_false<invocable_t>(), "Target is not invocable!");  \
      }                                                                        \
    }                                                                          \
  }

// Instantiate all CV/ref-qualified signature variants.
WH_DEFINE_SIMPLE_VTABLE(, );
WH_DEFINE_SIMPLE_VTABLE(const, );
WH_DEFINE_SIMPLE_VTABLE(, &);
WH_DEFINE_SIMPLE_VTABLE(, &&);
WH_DEFINE_SIMPLE_VTABLE(const, &);
WH_DEFINE_SIMPLE_VTABLE(const, &&);

WH_DEFINE_DESTRUCTIBLE_VTABLE(, );
WH_DEFINE_DESTRUCTIBLE_VTABLE(const, );
WH_DEFINE_DESTRUCTIBLE_VTABLE(, &);
WH_DEFINE_DESTRUCTIBLE_VTABLE(, &&);
WH_DEFINE_DESTRUCTIBLE_VTABLE(const, &);
WH_DEFINE_DESTRUCTIBLE_VTABLE(const, &&);

WH_DEFINE_CLONEABLE_VTABLE(, );
WH_DEFINE_CLONEABLE_VTABLE(const, );
WH_DEFINE_CLONEABLE_VTABLE(, &);
WH_DEFINE_CLONEABLE_VTABLE(, &&);
WH_DEFINE_CLONEABLE_VTABLE(const, &);
WH_DEFINE_CLONEABLE_VTABLE(const, &&);

// For vtable_handler: CV REF -> INV_QUALS mapping
// (no CV, no REF) -> &
// (const, no REF) -> const &
// (no CV, &) -> &
// (no CV, &&) -> &&
// (const, &) -> const &
// (const, &&) -> const &&
WH_DEFINE_VTABLE_HANDLER(, , &);
WH_DEFINE_VTABLE_HANDLER(const, , const &);
WH_DEFINE_VTABLE_HANDLER(, &, &);
WH_DEFINE_VTABLE_HANDLER(, &&, &&);
WH_DEFINE_VTABLE_HANDLER(const, &, const &);
WH_DEFINE_VTABLE_HANDLER(const, &&, const &&);

#undef WH_DEFINE_SIMPLE_VTABLE
#undef WH_DEFINE_DESTRUCTIBLE_VTABLE
#undef WH_DEFINE_CLONEABLE_VTABLE
#undef WH_DEFINE_VTABLE_HANDLER

} // namespace wh::core::fn_detail
