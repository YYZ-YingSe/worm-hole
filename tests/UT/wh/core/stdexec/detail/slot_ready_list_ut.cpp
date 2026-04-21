#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/stdexec/detail/slot_ready_list.hpp"

TEST_CASE("slot ready list publishes drains and resets slot ownership",
          "[UT][wh/core/stdexec/detail/"
          "slot_ready_list.hpp][slot_ready_list::publish][condition][branch][boundary]") {
  wh::core::detail::slot_ready_list ready{3U};

  REQUIRE_FALSE(ready.has_ready());
  REQUIRE(ready.publish(1U));
  REQUIRE_FALSE(ready.publish(1U));
  REQUIRE(ready.publish(0U));

  std::vector<std::uint32_t> drained{};
  ready.drain([&](const std::uint32_t slot_id) { drained.push_back(slot_id); });

  std::sort(drained.begin(), drained.end());
  REQUIRE(drained == std::vector<std::uint32_t>{0U, 1U});
  REQUIRE_FALSE(ready.has_ready());

  ready.reset(2U);
  REQUIRE_FALSE(ready.has_ready());
  REQUIRE(ready.publish(1U));
  drained.clear();
  ready.drain([&](const std::uint32_t slot_id) { drained.push_back(slot_id); });
  REQUIRE(drained == std::vector<std::uint32_t>{1U});
}

TEST_CASE("slot ready list drains concurrent slot publications without loss",
          "[UT][wh/core/stdexec/detail/"
          "slot_ready_list.hpp][slot_ready_list::drain][concurrency][branch]") {
  constexpr std::uint32_t slot_count = 8U;
  constexpr int rounds = 64;

  for (int round = 0; round < rounds; ++round) {
    wh::core::detail::slot_ready_list ready{slot_count};
    std::atomic<bool> start{false};
    std::atomic<std::uint32_t> published{0U};
    std::vector<std::thread> publishers{};
    publishers.reserve(slot_count);

    for (std::uint32_t slot = 0U; slot < slot_count; ++slot) {
      publishers.emplace_back([&, slot] {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        if (ready.publish(slot)) {
          published.fetch_add(1U, std::memory_order_acq_rel);
        }
      });
    }

    start.store(true, std::memory_order_release);

    std::vector<std::uint32_t> drained{};
    drained.reserve(slot_count);
    while (drained.size() < slot_count) {
      if (!ready.has_ready()) {
        std::this_thread::yield();
        continue;
      }
      ready.drain([&](const std::uint32_t slot_id) { drained.push_back(slot_id); });
    }

    for (auto &publisher : publishers) {
      publisher.join();
    }

    REQUIRE(published.load(std::memory_order_acquire) == slot_count);
    std::sort(drained.begin(), drained.end());
    REQUIRE(drained.size() == slot_count);
    for (std::uint32_t slot = 0U; slot < slot_count; ++slot) {
      REQUIRE(drained[slot] == slot);
    }
    REQUIRE_FALSE(ready.has_ready());
  }
}
