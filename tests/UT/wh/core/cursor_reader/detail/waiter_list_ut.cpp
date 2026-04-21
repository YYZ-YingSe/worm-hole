#include <catch2/catch_test_macros.hpp>

#include "wh/core/cursor_reader/detail/waiter_list.hpp"

namespace {

struct cursor_waiter {
  cursor_waiter *next{nullptr};
  cursor_waiter *prev{nullptr};
  int id{0};
};

} // namespace

TEST_CASE("cursor waiter list pushes removes and pops intrusive waiters",
          "[UT][wh/core/cursor_reader/detail/"
          "waiter_list.hpp][waiter_list::try_remove][condition][branch]") {
  wh::core::cursor_reader_detail::waiter_list<cursor_waiter> list{};
  cursor_waiter first{.id = 1};
  cursor_waiter second{.id = 2};
  cursor_waiter third{.id = 3};

  REQUIRE(list.empty());
  REQUIRE(list.front() == nullptr);
  REQUIRE(list.try_pop_front() == nullptr);
  REQUIRE_FALSE(list.try_remove(nullptr));

  list.push_back(&first);
  list.push_back(&second);
  list.push_back(&third);
  REQUIRE(list.front() == &first);
  REQUIRE(first.next == &second);
  REQUIRE(second.prev == &first);

  REQUIRE(list.try_remove(&second));
  REQUIRE(first.next == &third);
  REQUIRE(third.prev == &first);
  REQUIRE_FALSE(list.try_remove(&second));

  REQUIRE(list.try_pop_front() == &first);
  REQUIRE(list.front() == &third);
  REQUIRE(list.try_pop_front() == &third);
  REQUIRE(list.empty());
  REQUIRE(list.try_pop_front() == nullptr);
}

TEST_CASE("cursor waiter list removal handles head and tail waiters",
          "[UT][wh/core/cursor_reader/detail/"
          "waiter_list.hpp][waiter_list::push_back][branch][boundary]") {
  wh::core::cursor_reader_detail::waiter_list<cursor_waiter> list{};
  cursor_waiter first{.id = 1};
  cursor_waiter second{.id = 2};

  list.push_back(&first);
  list.push_back(&second);
  REQUIRE(list.try_remove(&first));
  REQUIRE(list.front() == &second);
  REQUIRE(second.prev == nullptr);
  REQUIRE(list.try_remove(&second));
  REQUIRE(list.empty());
}
