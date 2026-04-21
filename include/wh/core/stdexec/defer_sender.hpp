// Defines reusable deferred sender factories for internal async composition.
#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include <exec/completion_signatures.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/stdexec/result_sender.hpp"

namespace wh::core::detail {

template <typename factory_t> class defer_sender_impl {
  template <typename receiver_t> class operation {
    using child_sender_t = std::remove_cvref_t<std::invoke_result_t<factory_t &>>;
    using child_op_t = stdexec::connect_result_t<child_sender_t, receiver_t>;

  public:
    template <typename stored_factory_t>
      requires std::constructible_from<factory_t, stored_factory_t> &&
                   std::invocable<factory_t &> && stdexec::sender_to<child_sender_t, receiver_t>
    explicit operation(stored_factory_t &&factory, receiver_t receiver)
        : receiver_(std::move(receiver)), factory_(std::forward<stored_factory_t>(factory)) {}

    auto start() & noexcept -> void { start_child(); }

    ~operation() {
      if (child_started_) {
        std::destroy_at(child_op());
      }
    }

  private:
    [[nodiscard]] auto child_op() noexcept -> child_op_t * {
      return std::launder(reinterpret_cast<child_op_t *>(&child_op_storage_));
    }

    auto start_child() noexcept -> void {
      try {
        ::new (static_cast<void *>(child_op()))
            child_op_t(stdexec::connect(std::invoke(factory_), std::move(receiver_)));
        child_started_ = true;
      } catch (...) {
        stdexec::set_error(std::move(receiver_), std::current_exception());
        return;
      }
      stdexec::start(*child_op());
    }

    receiver_t receiver_;
    wh_no_unique_address factory_t factory_;
    alignas(child_op_t) std::byte child_op_storage_[sizeof(child_op_t)];
    bool child_started_{false};
  };

public:
  using sender_concept = stdexec::sender_t;

  template <typename self_t, stdexec::receiver receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, defer_sender_impl> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>) && std::invocable<factory_t &> &&
             stdexec::sender_to<std::remove_cvref_t<std::invoke_result_t<factory_t &>>, receiver_t>
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self, receiver_t receiver)
      -> operation<receiver_t> {
    return operation<receiver_t>{std::forward<self_t>(self).factory_, std::move(receiver)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>, defer_sender_impl> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>) && (sizeof...(env_t) == 1U) &&
             std::invocable<factory_t &> &&
             stdexec::sender<std::remove_cvref_t<std::invoke_result_t<factory_t &>>>
  static consteval auto get_completion_signatures() {
    using env_type = std::tuple_element_t<0U, std::tuple<env_t...>>;
    using child_sender_t = std::remove_cvref_t<std::invoke_result_t<factory_t &>>;

    auto value_fn = []<typename... value_t>() {
      return stdexec::completion_signatures<stdexec::set_value_t(value_t...)>{};
    };
    return exec::transform_completion_signatures(
        exec::get_child_completion_signatures<self_t, child_sender_t, env_type>(), value_fn,
        exec::keep_completion<stdexec::set_error_t>{},
        exec::keep_completion<stdexec::set_stopped_t>{},
        stdexec::completion_signatures<stdexec::set_error_t(std::exception_ptr)>{});
  }

  wh_no_unique_address factory_t factory_;
};

template <typename factory_t> [[nodiscard]] constexpr auto defer_sender(factory_t &&factory) {
  return defer_sender_impl<std::remove_cvref_t<factory_t>>{std::forward<factory_t>(factory)};
}

template <typename result_t, typename factory_t>
[[nodiscard]] constexpr auto defer_result_sender(factory_t &&factory) {
  return defer_sender([factory = std::forward<factory_t>(factory)]() mutable {
    return normalize_result_sender<result_t>(std::invoke(factory));
  });
}

} // namespace wh::core::detail
