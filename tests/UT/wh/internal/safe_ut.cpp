#include <catch2/catch_test_macros.hpp>

#include <new>
#include <stdexcept>

#include "wh/internal/safe.hpp"

TEST_CASE("safe_call returns values and void on successful execution",
          "[UT][wh/internal/safe.hpp][safe_call][branch][boundary]") {
  auto int_result = wh::internal::safe_call<int>([] { return 7; });
  REQUIRE(int_result.has_value());
  REQUIRE(int_result.value() == 7);

  auto void_result = wh::internal::safe_call<void>([] {});
  REQUIRE(void_result.has_value());
}

TEST_CASE("safe_call maps bad_alloc and generic exceptions to configured errors",
          "[UT][wh/internal/safe.hpp][safe_call][branch]") {
  auto oom = wh::internal::safe_call<int>([]() -> int { throw std::bad_alloc{}; });
  REQUIRE(oom.has_error());
  REQUIRE(oom.error() == wh::core::errc::resource_exhausted);

  auto failed = wh::internal::safe_call<int>(
      []() -> int { throw std::runtime_error{"boom"}; },
      wh::core::errc::canceled);
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::canceled);
}

TEST_CASE("safe_call maps void exceptions to explicit fallback errors",
          "[UT][wh/internal/safe.hpp][safe_call][condition][branch][boundary]") {
  auto failed = wh::internal::safe_call<void>(
      [] { throw std::runtime_error{"void-boom"}; },
      wh::core::errc::timeout);
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::timeout);
}
