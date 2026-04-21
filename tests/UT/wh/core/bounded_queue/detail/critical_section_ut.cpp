#include <mutex>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/bounded_queue/detail/critical_section.hpp"

TEST_CASE("bounded queue critical section aliases std mutex semantics",
          "[UT][wh/core/bounded_queue/detail/"
          "critical_section.hpp][bounded_queue_critical_section][branch]") {
  STATIC_REQUIRE(std::same_as<wh::core::detail::bounded_queue_critical_section, std::mutex>);

  wh::core::detail::bounded_queue_critical_section mutex{};
  std::lock_guard lock{mutex};
  SUCCEED();
}

TEST_CASE("bounded queue critical section supports unique_lock lifecycle",
          "[UT][wh/core/bounded_queue/detail/"
          "critical_section.hpp][bounded_queue_critical_section][condition][boundary]") {
  wh::core::detail::bounded_queue_critical_section mutex{};

  std::unique_lock first{mutex};
  REQUIRE(first.owns_lock());
  first.unlock();
  REQUIRE_FALSE(first.owns_lock());

  std::unique_lock second{mutex, std::defer_lock};
  REQUIRE_FALSE(second.owns_lock());
  second.lock();
  REQUIRE(second.owns_lock());
}
