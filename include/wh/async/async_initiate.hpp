#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/result.hpp"

namespace wh::core {

namespace detail {

template <typename result_t, stdexec::sender sender_t>
[[nodiscard]] auto make_awaitable_task(sender_t sender) -> exec::task<result_t>
  requires result_like<result_t>
{
  co_return co_await std::move(sender);
}

} // namespace detail

} // namespace wh::core
