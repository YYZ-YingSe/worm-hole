#include <sstream>
#include <tuple>
#include <type_traits>
#include <variant>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/core/status.hpp"

TEST_CASE("stream status exposes canonical signal text aliases and try-result shape",
          "[UT][wh/schema/stream/core/status.hpp][to_string][branch][boundary]") {
  using result_t = wh::schema::stream::stream_result<int>;
  using try_result_t = wh::schema::stream::stream_try_result<int>;

  STATIC_REQUIRE(std::same_as<result_t, wh::core::result<int>>);
  STATIC_REQUIRE(
      std::same_as<try_result_t, std::variant<wh::schema::stream::stream_signal, result_t>>);

  REQUIRE(wh::schema::stream::stream_pending == wh::schema::stream::stream_signal::pending);
  REQUIRE(wh::schema::stream::to_string(wh::schema::stream::stream_signal::pending) == "pending");

  std::ostringstream stream{};
  stream << wh::schema::stream::stream_signal::pending;
  REQUIRE(stream.str() == "pending");
}

TEST_CASE("stream status try-result can hold pending and ready branches",
          "[UT][wh/schema/stream/core/status.hpp][stream_try_result][condition]") {
  wh::schema::stream::stream_try_result<int> pending = wh::schema::stream::stream_pending;
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_signal>(pending));

  wh::schema::stream::stream_try_result<int> ready = wh::schema::stream::stream_result<int>{7};
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_result<int>>(ready));
  REQUIRE(std::get<wh::schema::stream::stream_result<int>>(ready).has_value());
  REQUIRE(std::get<wh::schema::stream::stream_result<int>>(ready).value() == 7);
}
