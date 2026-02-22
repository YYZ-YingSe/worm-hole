#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "wh/core/reflect.hpp"
#include "wh/internal/type_name.hpp"

namespace {

struct alias_alpha {};
struct alias_beta {};

} // namespace

namespace wh::internal {

template <> struct type_alias<alias_alpha> {
  static constexpr std::string_view value{"alias_alpha"};
};

template <> struct type_alias<alias_beta> {
  static constexpr std::string_view value{"alias_beta"};
};

} // namespace wh::internal

TEST_CASE("type_name alias registry resolves keys and aliases",
          "[core][type_name][condition]") {
  using registry_t = wh::internal::type_alias_registry<alias_alpha, alias_beta>;

  static_assert(wh::internal::has_explicit_type_alias_v<alias_alpha>);
  static_assert(wh::internal::has_explicit_type_alias_v<alias_beta>);

  constexpr auto alpha_hash = wh::internal::persistent_type_hash<alias_alpha>();
  constexpr auto beta_hash = wh::internal::persistent_type_hash<alias_beta>();
  static_assert(alpha_hash != 0);
  static_assert(beta_hash != 0);
  static_assert(alpha_hash != beta_hash);

  const auto alpha_lookup = registry_t::find_hash("alias_alpha");
  const auto beta_lookup = registry_t::find_hash("alias_beta");

  REQUIRE(alpha_lookup.has_value());
  REQUIRE(beta_lookup.has_value());
  REQUIRE(*alpha_lookup == alpha_hash);
  REQUIRE(*beta_lookup == beta_hash);
  REQUIRE(registry_t::find_alias(alpha_hash) == "alias_alpha");
  REQUIRE(registry_t::find_alias(beta_hash) == "alias_beta");
}

TEST_CASE("type_name stable name normalization keeps branch behavior",
          "[core][type_name][branch]") {
  REQUIRE(wh::internal::stable_function_name("  process_data  ") ==
          "process_data");
  REQUIRE(wh::internal::stable_runtime_type_name("  user_profile  ") ==
          "user_profile");

  REQUIRE(wh::internal::stable_function_name("lambda_42").empty());
  REQUIRE(wh::internal::stable_runtime_type_name("handler_99").empty());
}

TEST_CASE("type_name missing lookup and type_key extreme paths",
          "[core][type_name][extreme]") {
  using registry_t = wh::internal::type_alias_registry<alias_alpha, alias_beta>;

  REQUIRE_FALSE(registry_t::find_hash("missing_alias").has_value());
  REQUIRE(registry_t::find_alias(0xFFFFFFFFFFFFFFFFULL).empty());

  constexpr auto alpha_type_key = wh::core::make_type_key<alias_alpha>();
  REQUIRE(alpha_type_key.value ==
          wh::internal::persistent_type_hash<alias_alpha>());
}
