// Defines reusable result-oriented sender helpers shared by component and
// compose async paths.
#pragma once

#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/detail/receiver_stop_bridge.hpp"

namespace wh::core::detail {

template <typename result_t, stdexec::sender sender_t>
[[nodiscard]] constexpr auto normalize_result_sender(sender_t &&sender) {
  return std::forward<sender_t>(sender) |
         stdexec::upon_error([](auto &&) noexcept {
           return result_t::failure(wh::core::errc::internal_error);
         });
}

template <typename result_t> class result_sender final {
  using inner_completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(result_t),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;
  using inner_sender_t =
      typename exec::any_receiver_ref<
          inner_completion_signatures>::template any_sender<>;

public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(result_t),
                                     stdexec::set_stopped_t()>;

  template <typename sender_t>
    requires(!std::same_as<std::remove_cvref_t<sender_t>, result_sender> &&
             !std::same_as<std::remove_cvref_t<sender_t>, inner_sender_t>)
  /*implicit*/ result_sender(sender_t &&sender)
      : sender_(normalize_result_sender<result_t>(std::forward<sender_t>(sender))) {}

  result_sender(const result_sender &) = delete;
  auto operator=(const result_sender &) -> result_sender & = delete;
  result_sender(result_sender &&) noexcept = default;
  auto operator=(result_sender &&) noexcept -> result_sender & = default;
  ~result_sender() = default;

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  class operation {
    using bridge_t = wh::core::detail::receiver_stop_bridge<receiver_t>;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      bridge_t *bridge{nullptr};

      auto set_value(result_t status) && noexcept -> void {
        bridge->set_value(std::move(status));
      }

      auto set_error(std::exception_ptr) && noexcept -> void {
        bridge->set_value(result_t::failure(wh::core::errc::internal_error));
      }

      auto set_stopped() && noexcept -> void { bridge->set_stopped(); }

      [[nodiscard]] auto get_env() const noexcept ->
          typename bridge_t::stop_env_t {
        return bridge->env();
      }
    };

    using child_op_t = stdexec::connect_result_t<inner_sender_t, child_receiver>;

  public:
    using operation_state_concept = stdexec::operation_state_t;

    operation(inner_sender_t sender, receiver_t receiver)
        : bridge_(std::move(receiver)),
          child_op_(
              stdexec::connect(std::move(sender), child_receiver{std::addressof(bridge_)})) {}

    auto start() & noexcept -> void {
      if (!bridge_.bind_outer_stop()) {
        bridge_.set_stopped();
        return;
      }
      try {
        stdexec::start(child_op_);
      } catch (...) {
        bridge_.set_value(result_t::failure(wh::core::errc::internal_error));
      }
    }

  private:
    bridge_t bridge_;
    child_op_t child_op_;
  };

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  [[nodiscard]] auto connect(receiver_t receiver) && -> operation<receiver_t> {
    return operation<receiver_t>{std::move(sender_), std::move(receiver)};
  }

private:
  inner_sender_t sender_;
};

} // namespace wh::core::detail
