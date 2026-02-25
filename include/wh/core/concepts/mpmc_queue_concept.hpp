#pragma once

#include <concepts>
#include <cstddef>
#include <utility>

#include "wh/core/mpmc_queue.hpp"
#include "wh/core/result.hpp"

namespace wh::core {

template <typename queue_t>
concept mpmc_queue_like =
    std::constructible_from<queue_t, std::size_t> &&
    requires(queue_t &queue, typename queue_t::value_type value) {
      typename queue_t::value_type;
      { queue.try_push(value) } -> std::same_as<result<void>>;
      { queue.try_push(std::move(value)) } -> std::same_as<result<void>>;
      { queue.try_pop() } -> std::same_as<result<typename queue_t::value_type>>;
      { queue.empty() } -> std::convertible_to<bool>;
      { queue.capacity() } -> std::convertible_to<std::size_t>;
      { queue.lock_free() } -> std::convertible_to<bool>;
    };

static_assert(mpmc_queue_like<mpmc_queue<int>>);
static_assert(mpmc_queue_like<mpmc_queue<int, true>>);

} // namespace wh::core
