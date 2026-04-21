#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/cursor_reader/detail/waiter_ready_list.hpp"

namespace {

struct cursor_ready_waiter {
  cursor_ready_waiter *next{nullptr};
  cursor_ready_waiter *prev{nullptr};

  struct ops_t {
    void (*complete)(cursor_ready_waiter *) noexcept {nullptr};
  };

  const ops_t *ops{nullptr};
  int id{0};
  std::vector<int> *trace{nullptr};
};

auto cursor_ready_complete(cursor_ready_waiter *waiter) noexcept -> void {
  waiter->trace->push_back(waiter->id);
}

} // namespace

TEST_CASE("cursor waiter ready list completes waiters in insertion order",
          "[UT][wh/core/cursor_reader/detail/"
          "waiter_ready_list.hpp][waiter_ready_list::complete_all][condition][branch]") {
  wh::core::cursor_reader_detail::waiter_ready_list<cursor_ready_waiter> list{};
  std::vector<int> trace{};
  constexpr cursor_ready_waiter::ops_t ops{cursor_ready_complete};
  cursor_ready_waiter first{.ops = &ops, .id = 1, .trace = &trace};
  cursor_ready_waiter second{.ops = &ops, .id = 2, .trace = &trace};

  list.push_back(&first);
  list.push_back(&second);
  list.complete_all();

  REQUIRE(trace == std::vector<int>{1, 2});

  trace.clear();
  list.complete_all();
  REQUIRE(trace.empty());
  REQUIRE(first.next == nullptr);
  REQUIRE(second.next == nullptr);
}

TEST_CASE("cursor waiter ready list can be reused after drain",
          "[UT][wh/core/cursor_reader/detail/"
          "waiter_ready_list.hpp][waiter_ready_list::push_back][branch][boundary]") {
  wh::core::cursor_reader_detail::waiter_ready_list<cursor_ready_waiter> list{};
  std::vector<int> trace{};
  constexpr cursor_ready_waiter::ops_t ops{cursor_ready_complete};
  cursor_ready_waiter waiter{.next = reinterpret_cast<cursor_ready_waiter *>(0x1),
                             .prev = reinterpret_cast<cursor_ready_waiter *>(0x2),
                             .ops = &ops,
                             .id = 7,
                             .trace = &trace};

  list.push_back(&waiter);
  REQUIRE(waiter.next == nullptr);
  REQUIRE(waiter.prev == nullptr);
  list.complete_all();
  REQUIRE(trace == std::vector<int>{7});

  trace.clear();
  list.push_back(&waiter);
  list.complete_all();
  REQUIRE(trace == std::vector<int>{7});
}
