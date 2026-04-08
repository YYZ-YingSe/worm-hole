// Defines a movable terminal receiver completion that can be published by an
// operation and completed exactly once at the activation boundary.
#pragma once

#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"

namespace wh::core::detail {

struct receiver_stopped_completion_tag {};

template <typename receiver_t, typename value_t> class receiver_completion {
public:
  using receiver_type = std::remove_cvref_t<receiver_t>;
  using value_type = std::remove_cvref_t<value_t>;

  [[nodiscard]] static auto set_value(receiver_type receiver, value_type value)
      -> receiver_completion {
    return receiver_completion{
        std::move(receiver),
        std::variant<value_type, std::exception_ptr,
                     receiver_stopped_completion_tag>{
            std::in_place_type<value_type>, std::move(value)},
    };
  }

  [[nodiscard]] static auto
  set_error(receiver_type receiver, std::exception_ptr error)
      -> receiver_completion {
    return receiver_completion{
        std::move(receiver),
        std::variant<value_type, std::exception_ptr,
                     receiver_stopped_completion_tag>{
            std::in_place_type<std::exception_ptr>, std::move(error)},
    };
  }

  [[nodiscard]] static auto set_stopped(receiver_type receiver)
      -> receiver_completion {
    return receiver_completion{
        std::move(receiver),
        std::variant<value_type, std::exception_ptr,
                     receiver_stopped_completion_tag>{
            std::in_place_type<receiver_stopped_completion_tag>},
    };
  }

  receiver_completion(receiver_completion &&) noexcept = default;
  auto operator=(receiver_completion &&) noexcept -> receiver_completion & =
      default;

  receiver_completion(const receiver_completion &) = delete;
  auto operator=(const receiver_completion &) -> receiver_completion & = delete;

  auto complete() && noexcept -> void {
    auto receiver = std::move(receiver_);
    if (auto *value = std::get_if<value_type>(&payload_); value != nullptr) {
      stdexec::set_value(std::move(receiver), std::move(*value));
      return;
    }
    if (auto *error = std::get_if<std::exception_ptr>(&payload_);
        error != nullptr) {
      if constexpr (requires(receiver_type target, std::exception_ptr failure) {
                      stdexec::set_error(std::move(target),
                                         std::move(failure));
                    }) {
        stdexec::set_error(std::move(receiver), std::move(*error));
      } else {
        ::wh::core::contract_violation(
            ::wh::core::contract_kind::invariant,
            "receiver_completion error payload requires receiver set_error");
      }
      return;
    }
    if constexpr (requires(receiver_type target) {
                    stdexec::set_stopped(std::move(target));
                  }) {
      stdexec::set_stopped(std::move(receiver));
    } else {
      ::wh::core::contract_violation(
          ::wh::core::contract_kind::invariant,
          "receiver_completion stopped payload requires receiver set_stopped");
    }
  }

private:
  receiver_completion(
      receiver_type receiver,
      std::variant<value_type, std::exception_ptr,
                   receiver_stopped_completion_tag> payload) noexcept
      : receiver_(std::move(receiver)), payload_(std::move(payload)) {}

  receiver_type receiver_;
  std::variant<value_type, std::exception_ptr, receiver_stopped_completion_tag>
      payload_;
};

} // namespace wh::core::detail
