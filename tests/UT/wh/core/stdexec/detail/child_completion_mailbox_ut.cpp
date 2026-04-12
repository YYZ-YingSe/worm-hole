#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "wh/core/stdexec/detail/child_completion_mailbox.hpp"

TEST_CASE("child completion mailbox publishes drains and resets slot ownership",
          "[UT][wh/core/stdexec/detail/child_completion_mailbox.hpp][child_completion_mailbox::publish][condition][branch][boundary]") {
  wh::core::detail::child_completion_mailbox<int> mailbox{3U};

  REQUIRE_FALSE(mailbox.has_ready());
  REQUIRE(mailbox.publish(1U, 42));
  REQUIRE_FALSE(mailbox.publish(1U, 99));
  REQUIRE(mailbox.publish(0U, 7));

  std::vector<std::pair<std::uint32_t, int>> drained{};
  mailbox.drain([&](const std::uint32_t slot, int value) {
    drained.emplace_back(slot, value);
  });

  std::sort(drained.begin(), drained.end(),
            [](const auto &left, const auto &right) {
              return left.first < right.first;
            });
  REQUIRE(drained ==
          std::vector<std::pair<std::uint32_t, int>>{{0U, 7}, {1U, 42}});
  REQUIRE_FALSE(mailbox.has_ready());

  mailbox.reset(2U);
  REQUIRE_FALSE(mailbox.has_ready());
  REQUIRE(mailbox.publish(1U, 100));
  drained.clear();
  mailbox.drain([&](const std::uint32_t slot, int value) {
    drained.emplace_back(slot, value);
  });
  REQUIRE(drained ==
          std::vector<std::pair<std::uint32_t, int>>{{1U, 100}});
}

TEST_CASE("child completion mailbox drains concurrent slot publications without loss",
          "[UT][wh/core/stdexec/detail/child_completion_mailbox.hpp][child_completion_mailbox::drain][concurrency][branch]") {
  constexpr std::uint32_t slot_count = 8U;
  constexpr int rounds = 64;

  for (int round = 0; round < rounds; ++round) {
    wh::core::detail::child_completion_mailbox<int> mailbox{slot_count};
    std::atomic<bool> start{false};
    std::atomic<std::uint32_t> published{0U};
    std::vector<std::thread> publishers{};
    publishers.reserve(slot_count);

    for (std::uint32_t slot = 0U; slot < slot_count; ++slot) {
      publishers.emplace_back([&, slot] {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        if (mailbox.publish(slot, round * 100 + static_cast<int>(slot))) {
          published.fetch_add(1U, std::memory_order_acq_rel);
        }
      });
    }

    start.store(true, std::memory_order_release);

    std::vector<std::pair<std::uint32_t, int>> drained{};
    drained.reserve(slot_count);
    while (drained.size() < slot_count) {
      if (!mailbox.has_ready()) {
        std::this_thread::yield();
        continue;
      }
      mailbox.drain([&](const std::uint32_t slot, int value) {
        drained.emplace_back(slot, value);
      });
    }

    for (auto &publisher : publishers) {
      publisher.join();
    }

    REQUIRE(published.load(std::memory_order_acquire) == slot_count);
    std::sort(drained.begin(), drained.end(),
              [](const auto &left, const auto &right) {
                return left.first < right.first;
              });
    REQUIRE(drained.size() == slot_count);
    for (std::uint32_t slot = 0U; slot < slot_count; ++slot) {
      REQUIRE(drained[slot].first == slot);
      REQUIRE(drained[slot].second == round * 100 + static_cast<int>(slot));
    }
    REQUIRE_FALSE(mailbox.has_ready());
  }
}
