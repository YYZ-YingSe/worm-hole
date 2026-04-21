#include <catch2/catch_test_macros.hpp>

#include "wh/core/bounded_queue/detail/waiter_list.hpp"

namespace {

struct list_waiter {
  list_waiter *next{nullptr};
  list_waiter *prev{nullptr};
  int id{0};
};

} // namespace

TEST_CASE("waiter list push remove pop and detach preserve intrusive links",
          "[UT][wh/core/bounded_queue/detail/"
          "waiter_list.hpp][waiter_list::push_back][condition][branch]") {
  wh::core::detail::waiter_list<list_waiter> list{};
  list_waiter first{.id = 1};
  list_waiter second{.id = 2};
  list_waiter third{.id = 3};

  REQUIRE(list.empty());
  REQUIRE(list.front() == nullptr);
  REQUIRE_FALSE(list.try_remove(nullptr));

  list.push_back(&first);
  list.push_back(&second);
  list.push_back(&third);
  REQUIRE_FALSE(list.empty());
  REQUIRE(list.front() == &first);
  REQUIRE(second.prev == &first);
  REQUIRE(second.next == &third);

  REQUIRE(list.try_remove(&second));
  REQUIRE(first.next == &third);
  REQUIRE(third.prev == &first);
  REQUIRE(second.next == nullptr);
  REQUIRE(second.prev == nullptr);

  auto *popped = list.try_pop_front();
  REQUIRE(popped == &first);
  REQUIRE(list.front() == &third);
  REQUIRE_FALSE(list.try_remove(&first));

  auto *detached = list.detach_all();
  REQUIRE(detached == &third);
  REQUIRE(list.empty());
  REQUIRE(list.front() == nullptr);
}

TEST_CASE("waiter list handles singleton detach and empty-pop boundaries",
          "[UT][wh/core/bounded_queue/detail/waiter_list.hpp][waiter_list::detach_all][boundary]") {
  wh::core::detail::waiter_list<list_waiter> list{};
  list_waiter only{.id = 9};

  REQUIRE(list.try_pop_front() == nullptr);
  REQUIRE(list.detach_all() == nullptr);

  list.push_back(&only);
  auto *detached = list.detach_all();
  REQUIRE(detached == &only);
  REQUIRE(list.empty());
  REQUIRE(only.next == nullptr);
  REQUIRE(only.prev == nullptr);
  REQUIRE_FALSE(list.try_remove(&only));
}
