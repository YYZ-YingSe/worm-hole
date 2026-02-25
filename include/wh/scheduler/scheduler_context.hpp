#pragma once

#include <concepts>
#include <type_traits>

#include <stdexec/execution.hpp>

namespace wh::core {

template <typename scheduler_t>
concept stdexec_scheduler =
    stdexec::scheduler<std::remove_cvref_t<scheduler_t>>;

template <typename scheduler_t>
  requires stdexec_scheduler<scheduler_t>
struct scheduler_context {
  using execution_scheduler_type = std::remove_cvref_t<scheduler_t>;

  execution_scheduler_type execution_scheduler{};
};

template <typename type_t> struct is_scheduler_context : std::false_type {};

template <typename scheduler_t>
struct is_scheduler_context<scheduler_context<scheduler_t>> : std::true_type {};

template <typename context_t>
concept scheduler_context_like =
    is_scheduler_context<std::remove_cvref_t<context_t>>::value;

} // namespace wh::core
