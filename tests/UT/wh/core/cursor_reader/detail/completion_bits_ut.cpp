#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "wh/core/cursor_reader/detail/completion_bits.hpp"

TEST_CASE("completion bits claim and delivery flags are single-winner under contention",
          "[UT][wh/core/cursor_reader/detail/completion_bits.hpp][completion_bits::claim][concurrency][branch]") {
  constexpr int rounds = 64;
  constexpr int contenders = 8;

  for (int round = 0; round < rounds; ++round) {
    wh::core::cursor_reader_detail::completion_bits bits{};
    std::atomic<int> claims{0};
    std::atomic<int> deliveries{0};
    std::vector<std::thread> threads{};
    threads.reserve(contenders);

    for (int index = 0; index < contenders; ++index) {
      threads.emplace_back([&] {
        if (bits.claim()) {
          claims.fetch_add(1, std::memory_order_acq_rel);
        }
        if (bits.start_delivery()) {
          deliveries.fetch_add(1, std::memory_order_acq_rel);
        }
      });
    }

    for (auto &thread : threads) {
      thread.join();
    }

    REQUIRE(bits.has_claimed());
    REQUIRE(claims.load(std::memory_order_acquire) == 1);
    REQUIRE(deliveries.load(std::memory_order_acquire) == 1);
    REQUIRE_FALSE(bits.claim());
    REQUIRE_FALSE(bits.start_delivery());
  }
}

TEST_CASE("completion bits keep claim and delivery state independent across sequential calls",
          "[UT][wh/core/cursor_reader/detail/completion_bits.hpp][completion_bits::start_delivery][condition][branch][boundary]") {
  wh::core::cursor_reader_detail::completion_bits bits{};

  REQUIRE_FALSE(bits.has_claimed());
  REQUIRE(bits.start_delivery());
  REQUIRE_FALSE(bits.start_delivery());
  REQUIRE_FALSE(bits.has_claimed());

  REQUIRE(bits.claim());
  REQUIRE(bits.has_claimed());
  REQUIRE_FALSE(bits.claim());
}
