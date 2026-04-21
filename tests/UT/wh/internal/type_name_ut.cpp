#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "wh/internal/type_name.hpp"

namespace {

struct aliased_type {};

} // namespace

namespace wh::internal {

template <> struct type_alias<::aliased_type> {
  static constexpr std::string_view value = "aliased_type";
};

} // namespace wh::internal

TEST_CASE("type name helpers expose explicit aliases and stable hashes",
          "[UT][wh/internal/type_name.hpp][diagnostic_type_alias][branch][boundary]") {
  REQUIRE_FALSE(wh::internal::stable_type_name<int>().empty());
  REQUIRE(wh::internal::diagnostic_type_alias<aliased_type>() == "aliased_type");
  REQUIRE(wh::internal::persistent_type_alias<aliased_type>() == "aliased_type");
  REQUIRE(wh::internal::stable_name_hash("abc") == wh::internal::stable_name_hash("abc"));
  REQUIRE(wh::internal::stable_type_hash<aliased_type>() ==
          wh::internal::persistent_type_hash<aliased_type>());
}

TEST_CASE("type alias registry and runtime-name filters reject unstable generated names",
          "[UT][wh/internal/type_name.hpp][type_alias_registry][branch]") {
  using registry = wh::internal::type_alias_registry<aliased_type>;
  const auto hash = registry::find_hash("aliased_type");
  REQUIRE(hash.has_value());
  REQUIRE(registry::find_alias(*hash) == "aliased_type");

  REQUIRE(wh::internal::stable_function_name("  stable_name  ") == "stable_name");
  REQUIRE(wh::internal::stable_function_name("lambda_1").empty());
  REQUIRE(wh::internal::stable_function_name("generated_42").empty());

  REQUIRE(wh::internal::stable_runtime_type_name("  Type  ") == "Type");
  REQUIRE(wh::internal::stable_runtime_type_name("foo$12").empty());
}

TEST_CASE(
    "type name detail helpers classify ascii digits trim spaces and suffix markers",
    "[UT][wh/internal/type_name.hpp][detail::has_numeric_suffix][condition][branch][boundary]") {
  REQUIRE(wh::internal::detail::is_ascii_digit('7'));
  REQUIRE_FALSE(wh::internal::detail::is_ascii_digit('x'));
  REQUIRE(wh::internal::detail::trim_ascii_spaces(" \tname\t ") == "name");
  REQUIRE(wh::internal::detail::has_numeric_suffix("name_42"));
  REQUIRE(wh::internal::detail::has_numeric_suffix("name$7"));
  REQUIRE_FALSE(wh::internal::detail::has_numeric_suffix("stable_name"));
}
