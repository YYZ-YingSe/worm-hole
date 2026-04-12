#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_env.hpp"

namespace wh::testing::helper {

class static_thread_scheduler_helper {
public:
  using pool_type = exec::static_thread_pool;
  using scheduler_type = std::remove_cvref_t<
      decltype(std::declval<pool_type &>().get_scheduler())>;

  explicit static_thread_scheduler_helper(const std::uint32_t thread_count = 1U)
      : pool_(std::max<std::uint32_t>(thread_count, 1U)) {}

  static_thread_scheduler_helper(const static_thread_scheduler_helper &) =
      delete;
  auto operator=(const static_thread_scheduler_helper &)
      -> static_thread_scheduler_helper & = delete;

  static_thread_scheduler_helper(static_thread_scheduler_helper &&) = delete;
  auto operator=(static_thread_scheduler_helper &&)
      -> static_thread_scheduler_helper & = delete;

  [[nodiscard]] auto scheduler() -> scheduler_type {
    return pool_.get_scheduler();
  }

  [[nodiscard]] auto scheduler_on_thread(const std::size_t thread_index)
      -> scheduler_type {
    return pool_.get_scheduler_on_thread(thread_index);
  }

  template <typename stop_token_t = stdexec::never_stop_token>
  [[nodiscard]] auto env(stop_token_t stop_token = {})
      -> scheduler_env<scheduler_type, stop_token_t> {
    return make_scheduler_env(scheduler(), std::move(stop_token));
  }

private:
  pool_type pool_;
};

} // namespace wh::testing::helper
