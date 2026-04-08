#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "wh/core/bounded_queue/detail/waiter_ready_list.hpp"

namespace {

struct ready_waiter {
  ready_waiter *next{nullptr};
  ready_waiter *prev{nullptr};
  void (*complete)(ready_waiter *) noexcept {nullptr};
  int id{0};
  std::vector<int> *trace{nullptr};
};

auto record_completion(ready_waiter *waiter) noexcept -> void {
  waiter->trace->push_back(waiter->id);
}

} // namespace

TEST_CASE("waiter ready list completes queued waiters in FIFO order",
          "[UT][wh/core/bounded_queue/detail/waiter_ready_list.hpp][waiter_ready_list::complete_all][condition][branch]") {
  wh::core::detail::waiter_ready_list<ready_waiter> list{};
  std::vector<int> trace{};
  ready_waiter first{.complete = record_completion, .id = 1, .trace = &trace};
  ready_waiter second{.complete = record_completion, .id = 2, .trace = &trace};
  ready_waiter third{.complete = record_completion, .id = 3, .trace = &trace};

  list.push_back(&first);
  list.push_back(&second);
  list.push_back(&third);
  list.complete_all();

  REQUIRE(trace == std::vector<int>{1, 2, 3});

  trace.clear();
  list.complete_all();
  REQUIRE(trace.empty());
  REQUIRE(first.next == nullptr);
  REQUIRE(second.next == nullptr);
  REQUIRE(third.next == nullptr);
}

TEST_CASE("waiter ready list can be reused after draining and resets intrusive links on push",
          "[UT][wh/core/bounded_queue/detail/waiter_ready_list.hpp][waiter_ready_list::push_back][branch][boundary]") {
  wh::core::detail::waiter_ready_list<ready_waiter> list{};
  std::vector<int> trace{};
  ready_waiter waiter{.next = reinterpret_cast<ready_waiter *>(0x1),
                      .prev = reinterpret_cast<ready_waiter *>(0x2),
                      .complete = record_completion,
                      .id = 9,
                      .trace = &trace};

  list.push_back(&waiter);
  REQUIRE(waiter.next == nullptr);
  REQUIRE(waiter.prev == nullptr);
  list.complete_all();
  REQUIRE(trace == std::vector<int>{9});

  trace.clear();
  list.push_back(&waiter);
  list.complete_all();
  REQUIRE(trace == std::vector<int>{9});
}
