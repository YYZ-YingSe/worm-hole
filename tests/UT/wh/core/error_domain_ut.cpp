#include <catch2/catch_test_macros.hpp>

#include <new>
#include <stdexcept>

#include "wh/core/error_domain.hpp"

namespace {

using wh::core::errc;
using wh::core::exception_boundary;
using wh::core::map_current_exception;
using wh::core::map_exception;

} // namespace

TEST_CASE("map_exception classifies standard exception families",
          "[UT][wh/core/error_domain.hpp][map_exception][branch][boundary]") {
  REQUIRE(map_exception(std::bad_alloc{}) == errc::resource_exhausted);
  REQUIRE(map_exception(std::invalid_argument{"bad"}) ==
          errc::invalid_argument);
  REQUIRE(map_exception(std::out_of_range{"bad"}) == errc::invalid_argument);
  REQUIRE(map_exception(std::logic_error{"bad"}) ==
          errc::contract_violation);
  REQUIRE(map_exception(std::runtime_error{"boom"}) == errc::internal_error);
  REQUIRE(map_exception(std::exception{}) == errc::internal_error);
}

TEST_CASE("map_current_exception handles standard and unknown exceptions",
          "[UT][wh/core/error_domain.hpp][map_current_exception][condition][branch]") {
  try {
    throw std::invalid_argument{"bad"};
  } catch (...) {
    REQUIRE(map_current_exception() == errc::invalid_argument);
  }

  try {
    throw 7;
  } catch (...) {
    REQUIRE(map_current_exception() == errc::internal_error);
  }
}

TEST_CASE("exception_boundary returns success for non-throwing callables",
          "[UT][wh/core/error_domain.hpp][exception_boundary][branch]") {
  const auto value = exception_boundary<int>([] { return 42; });
  REQUIRE(value.has_value());
  REQUIRE(value.value() == 42);

  const auto unit = exception_boundary<void>([] {});
  REQUIRE(unit.has_value());
}

TEST_CASE("exception_boundary maps thrown exceptions into result errors",
          "[UT][wh/core/error_domain.hpp][exception_boundary][boundary]") {
  const auto invalid = exception_boundary<int>(
      []() -> int { throw std::invalid_argument{"bad"}; });
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == errc::invalid_argument);

  const auto oom = exception_boundary<void>(
      [] { throw std::bad_alloc{}; });
  REQUIRE(oom.has_error());
  REQUIRE(oom.error() == errc::resource_exhausted);

  const auto unknown = exception_boundary<int>([]() -> int { throw 7; });
  REQUIRE(unknown.has_error());
  REQUIRE(unknown.error() == errc::internal_error);
}
