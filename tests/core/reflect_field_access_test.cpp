#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <type_traits>

#include "wh/core/reflect.hpp"

namespace {

struct user_profile {
  int id{};
  std::string name{};
};

struct registry_alpha {};
struct registry_beta {};

} // namespace

namespace wh::internal {

template <> struct type_alias<registry_alpha> {
  static constexpr std::string_view value{"registry_alpha"};
};

template <> struct type_alias<registry_beta> {
  static constexpr std::string_view value{"registry_beta"};
};

} // namespace wh::internal

TEST_CASE("reflect field_map validates and exposes metadata",
          "[core][reflect][condition]") {
  constexpr auto id_field = wh::core::field("id", &user_profile::id);
  constexpr auto name_field = wh::core::field("name", &user_profile::name);

  static_assert(
      wh::core::validate_field_map<user_profile>(id_field, name_field));

  constexpr auto field_map =
      wh::core::make_field_map<user_profile>(id_field, name_field);
  REQUIRE(field_map.size() == 2);

  const auto names = field_map.names();
  REQUIRE(names[0] == "id");
  REQUIRE(names[1] == "name");

  const auto keys = field_map.keys();
  REQUIRE(keys[0] != 0);
  REQUIRE(keys[1] != 0);
  REQUIRE(keys[0] != keys[1]);
}

TEST_CASE("reflect visit_field supports found and not_found branches",
          "[core][reflect][branch]") {
  constexpr auto id_field = wh::core::field("id", &user_profile::id);
  constexpr auto name_field = wh::core::field("name", &user_profile::name);
  constexpr auto field_map =
      wh::core::make_field_map<user_profile>(id_field, name_field);

  user_profile profile{.id = 1, .name = "alice"};

  const bool found_id =
      wh::core::visit_field(field_map, "id", [&](const auto &binding) {
        using field_t =
            typename std::remove_cvref_t<decltype(binding)>::value_type;
        if constexpr (std::is_same_v<field_t, int>) {
          wh::core::field_ref(profile, binding) = 42;
        }
      });

  const bool found_missing =
      wh::core::visit_field(field_map, "missing", [&](const auto &) {
        FAIL("missing field should not be visited");
      });

  REQUIRE(found_id);
  REQUIRE_FALSE(found_missing);
  REQUIRE(profile.id == 42);
}

TEST_CASE("reflect key lookup stays stable", "[core][reflect][extreme]") {
  constexpr auto id_field = wh::core::field("id", &user_profile::id);
  constexpr auto name_field = wh::core::field("name", &user_profile::name);
  constexpr auto field_map =
      wh::core::make_field_map<user_profile>(id_field, name_field);

  user_profile profile{.id = 3, .name = "bob"};

  const auto key = wh::internal::stable_name_hash("name");
  const bool found_by_key =
      wh::core::visit_field_by_key(field_map, key, [&](const auto &binding) {
        using field_t =
            typename std::remove_cvref_t<decltype(binding)>::value_type;
        if constexpr (std::is_same_v<field_t, std::string>) {
          wh::core::field_ref(profile, binding) = "carol";
        }
      });

  REQUIRE(found_by_key);
  REQUIRE(profile.name == "carol");
}

TEST_CASE("reflect type key registry lookup contract",
          "[core][reflect][extreme]") {
  constexpr auto alpha_key = wh::core::make_type_key<registry_alpha>();
  constexpr auto beta_key = wh::core::make_type_key<registry_beta>();

  REQUIRE(alpha_key.value != 0);
  REQUIRE(beta_key.value != 0);
  REQUIRE(alpha_key.value != beta_key.value);

  const auto found_alpha =
      wh::core::find_type_key<registry_alpha, registry_beta>("registry_alpha");
  const auto found_beta =
      wh::core::find_type_key<registry_alpha, registry_beta>("registry_beta");
  const auto missing =
      wh::core::find_type_key<registry_alpha, registry_beta>("missing");

  REQUIRE(found_alpha.has_value());
  REQUIRE(found_beta.has_value());
  REQUIRE_FALSE(missing.has_value());

  REQUIRE(wh::core::find_type_alias<registry_alpha, registry_beta>(alpha_key) ==
          "registry_alpha");
  REQUIRE(wh::core::find_type_alias<registry_alpha, registry_beta>(beta_key) ==
          "registry_beta");
}
