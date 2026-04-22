// Defines shared sender metadata helpers used to probe completion signatures.
#pragma once

#include <concepts>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::core::detail {

template <typename signature_t> struct set_value_signature : std::false_type {
  using result_type = void;
};

template <typename result_t>
struct set_value_signature<stdexec::set_value_t(result_t)> : std::true_type {
  using result_type = result_t;
};

namespace sender_meta_detail {

template <typename current_t, typename... signatures_t> struct first_set_value_result {
  using type = current_t;
};

template <typename current_t, typename signature_t, typename... rest_t>
struct first_set_value_result<current_t, signature_t, rest_t...> {
  using next_t = std::conditional_t<set_value_signature<signature_t>::value,
                                    typename set_value_signature<signature_t>::result_type,
                                    current_t>;
  using type = typename first_set_value_result<next_t, rest_t...>::type;
};

} // namespace sender_meta_detail

template <typename signatures_t> struct single_set_value_signature : std::false_type {
  using result_type = void;
  static constexpr std::size_t set_value_count = 0U;
};

template <typename... signatures_t>
struct single_set_value_signature<stdexec::completion_signatures<signatures_t...>>
    : std::bool_constant<
          (0U + ... + static_cast<std::size_t>(set_value_signature<signatures_t>::value)) == 1U> {
  static constexpr std::size_t set_value_count =
      (0U + ... + static_cast<std::size_t>(set_value_signature<signatures_t>::value));
  using result_type = std::conditional_t<
      set_value_count == 1U,
      typename sender_meta_detail::first_set_value_result<void, signatures_t...>::type, void>;
};

template <typename sender_t, typename env_t = sender_signature_env>
  requires stdexec::sender<std::remove_cvref_t<sender_t>>
using sender_completion_signatures_t =
    stdexec::completion_signatures_of_t<std::remove_cvref_t<sender_t>, std::remove_cvref_t<env_t>>;

template <typename sender_t, typename env_t = sender_signature_env>
using sender_value_signature = single_set_value_signature<sender_completion_signatures_t<sender_t, env_t>>;

template <typename sender_t, typename result_t, typename env_t = sender_signature_env>
concept sender_exact_value =
    stdexec::sender<std::remove_cvref_t<sender_t>> &&
    sender_value_signature<sender_t, env_t>::value &&
    std::same_as<typename sender_value_signature<sender_t, env_t>::result_type, result_t>;

} // namespace wh::core::detail
