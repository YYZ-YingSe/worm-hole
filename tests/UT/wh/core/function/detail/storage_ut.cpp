#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "wh/core/function/detail/storage.hpp"

namespace {

struct alignas(16) over_aligned_value {
  int value{0};
};

} // namespace

TEST_CASE("any_storage head and address helpers preserve sentinel state",
          "[UT][wh/core/function/detail/storage.hpp][any_storage::head][condition][branch]") {
  wh::core::fn_detail::any_storage<8> storage{};
  REQUIRE(storage.head() == nullptr);
  REQUIRE(storage.address() != nullptr);
  REQUIRE(storage.address() == static_cast<const void *>(storage.data_));

  storage.head() = storage.address();
  REQUIRE(storage.head() == storage.address());
  storage = nullptr;
  REQUIRE(storage.head() == nullptr);
}

TEST_CASE("any_storage interpret_as reads mutable and const in-place objects",
          "[UT][wh/core/function/detail/storage.hpp][any_storage::interpret_as][branch][boundary]") {
  wh::core::fn_detail::any_storage<8> storage{};
  new (storage.address()) int(7);
  REQUIRE(storage.interpret_as<int &>() == 7);
  REQUIRE(storage.interpret_as<const int &>() == 7);
  storage.interpret_as<int &>() = 9;
  REQUIRE(storage.interpret_as<const int &>() == 9);
}

TEST_CASE("any_storage can_construct rejects oversized and over-aligned targets",
          "[UT][wh/core/function/detail/storage.hpp][any_storage::can_construct][condition][boundary]") {
  struct larger_value {
    int payload[4]{};
  };

  STATIC_REQUIRE(wh::core::fn_detail::any_storage<8>::can_construct<int>());
  STATIC_REQUIRE(!wh::core::fn_detail::any_storage<0>::can_construct<int>());
  STATIC_REQUIRE(!wh::core::fn_detail::any_storage<8>::can_construct<
                 larger_value>());
  STATIC_REQUIRE(!wh::core::fn_detail::any_storage<8>::can_construct<
                 over_aligned_value>());
}
