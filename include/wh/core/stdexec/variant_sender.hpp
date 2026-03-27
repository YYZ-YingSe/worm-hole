// Defines a lightweight typed sender variant for internal branch composition.
#pragma once

#include <concepts>
#include <exec/completion_signatures.hpp>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace wh::core::detail {

namespace variant_sender_detail {

template <typename from_t, typename to_t>
using copy_const_t =
    std::conditional_t<std::is_const_v<std::remove_reference_t<from_t>>,
                       std::add_const_t<to_t>, to_t>;

template <typename from_t, typename to_t>
using copy_cvref_t =
    std::conditional_t<std::is_lvalue_reference_v<from_t>,
                       std::add_lvalue_reference_t<copy_const_t<from_t, to_t>>,
                       std::add_rvalue_reference_t<copy_const_t<from_t, to_t>>>;

template <typename target_t, typename... candidate_t>
inline constexpr std::size_t same_type_count_v =
    (0U + ... + static_cast<std::size_t>(std::same_as<target_t, candidate_t>));

template <typename target_t, typename first_t, typename... rest_t>
consteval auto first_type_index() -> std::size_t {
  if constexpr (std::same_as<target_t, first_t>) {
    return 0U;
  } else {
    static_assert(sizeof...(rest_t) > 0U,
                  "requested type is not present in variant_sender");
    return 1U + first_type_index<target_t, rest_t...>();
  }
}

} // namespace variant_sender_detail

template <stdexec::sender... sender_t> class variant_sender {
  template <typename receiver_t, typename... operation_t>
  class operation_state {
    using storage_tuple_t =
        std::tuple<wh::core::detail::manual_lifetime_box<operation_t>...>;

  public:
    template <typename self_t>
    explicit operation_state(self_t &&self, receiver_t receiver) {
      connect_active<0U>(std::forward<self_t>(self), receiver);
    }

    auto start() & noexcept -> void { start_active<0U>(); }

  private:
    template <std::size_t Index, typename self_t>
    auto connect_active(self_t &&self, receiver_t &receiver) -> void {
      if constexpr (Index < sizeof...(sender_t)) {
        if (self.index() == Index) {
          std::get<Index>(operations_)
              .emplace_from(
                  stdexec::connect,
                  std::get<Index + 1U>(std::forward<self_t>(self).senders_),
                  std::move(receiver));
          active_index_ = Index;
          return;
        }
        connect_active<Index + 1U>(std::forward<self_t>(self), receiver);
      }
    }

    template <std::size_t Index> auto start_active() noexcept -> void {
      if constexpr (Index < sizeof...(sender_t)) {
        if (active_index_ == Index) {
          stdexec::start(std::get<Index>(operations_).get());
          return;
        }
        start_active<Index + 1U>();
      }
    }

    storage_tuple_t operations_{};
    std::size_t active_index_{static_cast<std::size_t>(-1)};
  };

public:
  using sender_concept = stdexec::sender_t;

  variant_sender()
    requires std::default_initializable<
        std::tuple_element_t<0U, std::tuple<sender_t...>>>
  {
    senders_.template emplace<1U>();
  }

  template <std::size_t Index, typename... args_t>
    requires(Index < sizeof...(sender_t))
  explicit variant_sender(std::in_place_index_t<Index>, args_t &&...args) {
    senders_.template emplace<Index + 1U>(std::forward<args_t>(args)...);
  }

  template <typename selected_sender_t>
    requires(!std::same_as<std::remove_cvref_t<selected_sender_t>,
                           variant_sender>) &&
            (variant_sender_detail::same_type_count_v<
                 std::remove_cvref_t<selected_sender_t>, sender_t...> == 1U)
  /*implicit*/ variant_sender(selected_sender_t &&sender) {
    constexpr auto index = variant_sender_detail::first_type_index<
        std::remove_cvref_t<selected_sender_t>, sender_t...>();
    senders_.template emplace<index + 1U>(
        std::forward<selected_sender_t>(sender));
  }

  template <typename selected_sender_t>
    requires(!std::same_as<std::remove_cvref_t<selected_sender_t>,
                           variant_sender>) &&
            (variant_sender_detail::same_type_count_v<
                 std::remove_cvref_t<selected_sender_t>, sender_t...> == 1U)
  auto operator=(selected_sender_t &&sender) -> variant_sender & {
    constexpr auto index = variant_sender_detail::first_type_index<
        std::remove_cvref_t<selected_sender_t>, sender_t...>();
    senders_.template emplace<index + 1U>(
        std::forward<selected_sender_t>(sender));
    return *this;
  }

  template <std::size_t Index, typename... args_t>
    requires(Index < sizeof...(sender_t))
  auto emplace(args_t &&...args) -> decltype(auto) {
    return senders_.template emplace<Index + 1U>(std::forward<args_t>(args)...);
  }

  [[nodiscard]] auto index() const noexcept -> std::size_t {
    return senders_.index() - 1U;
  }

  template <typename self_t, stdexec::receiver receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, variant_sender> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>) &&
             (stdexec::sender_to<
                  variant_sender_detail::copy_cvref_t<self_t, sender_t>,
                  receiver_t> &&
              ...)
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                            receiver_t receiver) {
    using state_t = operation_state<
        receiver_t, stdexec::connect_result_t<
                        variant_sender_detail::copy_cvref_t<self_t, sender_t>,
                        receiver_t>...>;
    return state_t{std::forward<self_t>(self), std::move(receiver)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>, variant_sender> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>) &&
             (sizeof...(env_t) >= 1U)
  static consteval auto get_completion_signatures() {
    return exec::concat_completion_signatures(
        stdexec::get_completion_signatures<
            variant_sender_detail::copy_cvref_t<self_t, sender_t>,
            env_t...>()...);
  }

private:
  template <typename receiver_t, typename... operation_t>
  friend class operation_state;

  std::variant<std::monostate, sender_t...> senders_{std::in_place_index<0U>};
};

} // namespace wh::core::detail
