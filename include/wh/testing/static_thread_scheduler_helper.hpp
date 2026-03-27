#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <exec/static_thread_pool.hpp>

namespace wh::testing {

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

private:
  pool_type pool_;
};

} // namespace wh::testing
