#include <cstdint>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/cursor_reader/detail/pull_state.hpp"

TEST_CASE("pull state exposes stable ordered lifecycle phases",
          "[UT][wh/core/cursor_reader/detail/pull_state.hpp][pull_state][branch][boundary]") {
  using pull_state = wh::core::cursor_reader_detail::pull_state;

  STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<pull_state>, std::uint8_t>);
  REQUIRE(static_cast<std::uint8_t>(pull_state::idle) == 0U);
  REQUIRE(static_cast<std::uint8_t>(pull_state::try_reading) == 1U);
  REQUIRE(static_cast<std::uint8_t>(pull_state::blocking_reading) == 2U);
  REQUIRE(static_cast<std::uint8_t>(pull_state::async_reading) == 3U);
  REQUIRE(static_cast<std::uint8_t>(pull_state::async_draining) == 4U);
  REQUIRE(static_cast<std::uint8_t>(pull_state::closing) == 5U);
  REQUIRE(static_cast<std::uint8_t>(pull_state::terminal) == 6U);
}

TEST_CASE("pull state values remain strictly monotonic from idle to terminal",
          "[UT][wh/core/cursor_reader/detail/pull_state.hpp][pull_state][condition]") {
  using pull_state = wh::core::cursor_reader_detail::pull_state;

  REQUIRE(static_cast<std::uint8_t>(pull_state::idle) <
          static_cast<std::uint8_t>(pull_state::try_reading));
  REQUIRE(static_cast<std::uint8_t>(pull_state::try_reading) <
          static_cast<std::uint8_t>(pull_state::blocking_reading));
  REQUIRE(static_cast<std::uint8_t>(pull_state::blocking_reading) <
          static_cast<std::uint8_t>(pull_state::async_reading));
  REQUIRE(static_cast<std::uint8_t>(pull_state::async_reading) <
          static_cast<std::uint8_t>(pull_state::async_draining));
  REQUIRE(static_cast<std::uint8_t>(pull_state::async_draining) <
          static_cast<std::uint8_t>(pull_state::closing));
  REQUIRE(static_cast<std::uint8_t>(pull_state::closing) <
          static_cast<std::uint8_t>(pull_state::terminal));
}
