#include <string>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/core/stdexec/variant_sender.hpp"

TEST_CASE("variant sender default constructor assignment and emplace select branches",
          "[UT][wh/core/stdexec/variant_sender.hpp][variant_sender::emplace][branch][boundary]") {
  constexpr auto append_done = +[](std::string value) { return value + "-done"; };

  using sender_t =
      wh::core::detail::variant_sender<decltype(stdexec::just(std::string{"branch-a"})),
                                       decltype(stdexec::just(std::string{"branch-b"}) |
                                                stdexec::then(append_done))>;

  STATIC_REQUIRE(std::default_initializable<sender_t>);
  STATIC_REQUIRE(stdexec::sender<sender_t>);

  sender_t sender{};
  REQUIRE(sender.index() == 0U);
  REQUIRE(wh::testing::helper::wait_value_on_test_thread(sender).empty());

  sender = stdexec::just(std::string{"reassigned-a"});
  REQUIRE(sender.index() == 0U);
  REQUIRE(wh::testing::helper::wait_value_on_test_thread(sender) == "reassigned-a");

  sender.emplace<1U>(stdexec::just(std::string{"branch-b"}) | stdexec::then(append_done));
  REQUIRE(sender.index() == 1U);
  REQUIRE(wh::testing::helper::wait_value_on_test_thread(std::move(sender)) == "branch-b-done");
}

TEST_CASE("variant sender chooses only the active runtime branch",
          "[UT][wh/core/stdexec/variant_sender.hpp][variant_sender::connect][condition][branch]") {
  bool first_called = false;
  bool second_called = false;
  constexpr auto plus_one = +[](int value) { return value + 1; };
  using sender_t =
      wh::core::detail::variant_sender<decltype(stdexec::just(1)),
                                       decltype(stdexec::just(2) | stdexec::then(plus_one))>;

  const auto first_status = wh::testing::helper::wait_value_on_test_thread([&]() -> sender_t {
    if (true) {
      first_called = true;
      return sender_t{std::in_place_index<0U>, stdexec::just(1)};
    }
    second_called = true;
    return sender_t{std::in_place_index<1U>, stdexec::just(2) | stdexec::then(plus_one)};
  }());
  REQUIRE(first_called);
  REQUIRE_FALSE(second_called);
  REQUIRE(first_status == 1);

  first_called = false;
  second_called = false;
  const auto second_status = wh::testing::helper::wait_value_on_test_thread([&]() -> sender_t {
    if (false) {
      first_called = true;
      return sender_t{std::in_place_index<0U>, stdexec::just(1)};
    }
    second_called = true;
    return sender_t{std::in_place_index<1U>, stdexec::just(2) | stdexec::then(plus_one)};
  }());
  REQUIRE_FALSE(first_called);
  REQUIRE(second_called);
  REQUIRE(second_status == 3);
}
