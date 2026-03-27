// Defines shared base logic for function wrappers, including invocation
// dispatch, storage ownership, and target state management.
#pragma once

#include <type_traits>
#include <utility>

#include "wh/core/function/detail/storage_policy.hpp"
#include "wh/core/function/detail/utils.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::core::fn_detail {

/// Base storage wrapper for callable objects.
template <typename storage_t>
class function_base : public storage_t {
protected:
  using storage_type = storage_t;
  typename storage_t::buffer_type local_buffer_{nullptr};

  function_base() noexcept = default;

  /// Checks whether in-place construction is `noexcept`.
  template <typename fun_t, typename... args_t>
  [[nodiscard]] static consteval auto check_nothrow() noexcept -> bool {
    return storage_t::template traits<fun_t>::template is_nothrow_constructible<
        args_t...>;
  }

  template <typename fun_t, typename... args_t>
  explicit function_base(std::in_place_type_t<fun_t>,
                         args_t &&...args) noexcept(check_nothrow<std::decay_t<fun_t>,
                                                                   args_t...>()) {
    if constexpr (sizeof...(args_t) == 1U) {
      if constexpr (is_function_pointer_v<wh::core::remove_cvref_t<fun_t>> ||
                    std::is_member_pointer_v<std::decay_t<fun_t>>) {
        if ((args == ... == nullptr)) {
          return;
        }
      }
    }

    storage_t::template create<fun_t>(local_buffer_,
                                      std::forward<args_t>(args)...);
  }

  ~function_base() { storage_t::destroy(local_buffer_); }

  function_base(const function_base &other) {
    storage_t::copy(other.local_buffer_, local_buffer_);
  }

  function_base(function_base &&other) noexcept
      : local_buffer_(std::move(other.local_buffer_)) {
    storage_t::set_to_empty(other.local_buffer_);
  }

  explicit function_base(std::nullptr_t) noexcept {
    storage_t::set_to_empty(local_buffer_);
  }

  auto operator=(const function_base &other) -> function_base & {
    function_base{other}.swap(*this);
    return *this;
  }

  auto operator=(function_base &&other) noexcept -> function_base & {
    storage_t::destroy(local_buffer_);
    local_buffer_ = std::move(other.local_buffer_);
    storage_t::set_to_empty(other.local_buffer_);

    return *this;
  }

  auto operator=(std::nullptr_t) noexcept -> function_base & {
    storage_t::destroy(local_buffer_);
    storage_t::set_to_empty(local_buffer_);

    return *this;
  }

  /// Swaps callable storage state with another instance.
  auto swap(function_base &other) noexcept -> void {
    typename storage_t::buffer_type tmp_buffer{nullptr};

    tmp_buffer = std::move(other.local_buffer_);
    other.local_buffer_ = std::move(local_buffer_);
    local_buffer_ = std::move(tmp_buffer);
  }
};

} // namespace wh::core::fn_detail
