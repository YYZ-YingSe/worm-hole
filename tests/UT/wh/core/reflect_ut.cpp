#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "wh/core/reflect.hpp"

namespace {

struct test_profile {
  int id{};
  std::string name{};
  bool enabled{};
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

TEST_CASE("reflect field binding and validation cover name and key constraints",
          "[UT][wh/core/reflect.hpp][field][condition][boundary]") {
  constexpr auto id_field = wh::core::field("id", &test_profile::id);
  constexpr auto name_field = wh::core::field("name", &test_profile::name);

  REQUIRE(id_field.name == "id");
  REQUIRE(id_field.key == wh::internal::stable_name_hash("id"));
  REQUIRE(name_field.key == wh::internal::stable_name_hash("name"));

  constexpr wh::core::field_binding<test_profile, int> empty_name{
      "", 1U, &test_profile::id};
  constexpr wh::core::field_binding<test_profile, int> duplicate_name{
      "id", 2U, &test_profile::id};
  constexpr wh::core::field_binding<test_profile, std::string> duplicate_key{
      "name", id_field.key, &test_profile::name};

  STATIC_REQUIRE(
      wh::core::validate_field_map<test_profile>(id_field, name_field));
  STATIC_REQUIRE_FALSE(
      wh::core::validate_field_map<test_profile>(empty_name, name_field));
  STATIC_REQUIRE_FALSE(
      wh::core::validate_field_map<test_profile>(id_field, duplicate_name));
  STATIC_REQUIRE_FALSE(
      wh::core::validate_field_map<test_profile>(id_field, duplicate_key));
}

TEST_CASE("reflect field map exposes names keys and member references",
          "[UT][wh/core/reflect.hpp][make_field_map][branch][boundary]") {
  constexpr auto id_field = wh::core::field("id", &test_profile::id);
  constexpr auto name_field = wh::core::field("name", &test_profile::name);
  constexpr auto enabled_field =
      wh::core::field("enabled", &test_profile::enabled);
  constexpr auto field_map = wh::core::make_field_map<test_profile>(
      id_field, name_field, enabled_field);

  STATIC_REQUIRE(std::same_as<decltype(field_map)::owner_type, test_profile>);
  REQUIRE(field_map.size() == 3U);
  REQUIRE(field_map.names() ==
          std::array<std::string_view, 3U>{"id", "name", "enabled"});

  const auto keys = field_map.keys();
  REQUIRE(keys[0] != 0U);
  REQUIRE(keys[1] != 0U);
  REQUIRE(keys[2] != 0U);
  REQUIRE(keys[0] != keys[1]);
  REQUIRE(keys[1] != keys[2]);

  test_profile profile{.id = 7, .name = "alice", .enabled = false};
  wh::core::field_ref(profile, id_field) = 42;
  wh::core::field_ref(profile, name_field) = "bob";
  wh::core::field_ref(profile, enabled_field) = true;

  const test_profile &const_profile = profile;
  REQUIRE(wh::core::field_ref(const_profile, id_field) == 42);
  REQUIRE(wh::core::field_ref(const_profile, name_field) == "bob");
  REQUIRE(wh::core::field_ref(const_profile, enabled_field));
}

TEST_CASE("reflect field iteration and lookup cover hit miss and key branches",
          "[UT][wh/core/reflect.hpp][visit_field][condition][branch]") {
  constexpr auto id_field = wh::core::field("id", &test_profile::id);
  constexpr auto name_field = wh::core::field("name", &test_profile::name);
  constexpr auto enabled_field =
      wh::core::field("enabled", &test_profile::enabled);
  constexpr auto field_map = wh::core::make_field_map<test_profile>(
      id_field, name_field, enabled_field);

  std::vector<std::string_view> names{};
  wh::core::for_each_field(field_map,
                           [&](const auto &binding) { names.push_back(binding.name); });
  REQUIRE(names ==
          std::vector<std::string_view>{"id", "name", "enabled"});

  test_profile profile{.id = 1, .name = "carol", .enabled = false};
  bool id_branch_called = false;
  bool name_branch_called = false;

  const bool found_id =
      wh::core::visit_field(field_map, "id", [&](const auto &binding) {
        using binding_t = std::remove_cvref_t<decltype(binding)>;
        if constexpr (std::same_as<typename binding_t::value_type, int>) {
          id_branch_called = true;
          wh::core::field_ref(profile, binding) = 9;
        } else {
          name_branch_called = true;
        }
      });
  REQUIRE(found_id);
  REQUIRE(id_branch_called);
  REQUIRE_FALSE(name_branch_called);
  REQUIRE(profile.id == 9);

  const bool found_missing =
      wh::core::visit_field(field_map, "missing", [&](const auto &) {
        FAIL("missing field must not invoke callback");
      });
  REQUIRE_FALSE(found_missing);

  bool found_name_by_key = false;
  const auto name_key = wh::internal::stable_name_hash("name");
  const bool found_key = wh::core::visit_field_by_key(
      field_map, name_key, [&](const auto &binding) {
        using binding_t = std::remove_cvref_t<decltype(binding)>;
        if constexpr (std::same_as<typename binding_t::value_type, std::string>) {
          found_name_by_key = true;
          wh::core::field_ref(profile, binding) = "dave";
        }
      });
  REQUIRE(found_key);
  REQUIRE(found_name_by_key);
  REQUIRE(profile.name == "dave");

  const bool missing_key = wh::core::visit_field_by_key(
      field_map, 0U, [&](const auto &) { FAIL("unknown key must not be visited"); });
  REQUIRE_FALSE(missing_key);
}

TEST_CASE("reflect type key helpers keep alias lookup stable",
          "[UT][wh/core/reflect.hpp][find_type_key][branch][boundary]") {
  constexpr auto alpha_key = wh::core::make_type_key<registry_alpha>();
  constexpr auto beta_key = wh::core::make_type_key<registry_beta>();

  STATIC_REQUIRE(
      std::same_as<std::remove_cvref_t<decltype(alpha_key)>, wh::core::type_key>);
  REQUIRE(alpha_key.value != 0U);
  REQUIRE(beta_key.value != 0U);
  REQUIRE(alpha_key != beta_key);

  const auto found_alpha =
      wh::core::find_type_key<registry_alpha, registry_beta>("registry_alpha");
  const auto found_beta =
      wh::core::find_type_key<registry_alpha, registry_beta>("registry_beta");
  const auto missing =
      wh::core::find_type_key<registry_alpha, registry_beta>("missing");

  REQUIRE(found_alpha.has_value());
  REQUIRE(found_beta.has_value());
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(*found_alpha == alpha_key);
  REQUIRE(*found_beta == beta_key);

  REQUIRE(wh::core::find_type_alias<registry_alpha, registry_beta>(alpha_key) ==
          "registry_alpha");
  REQUIRE(wh::core::find_type_alias<registry_alpha, registry_beta>(beta_key) ==
          "registry_beta");
  REQUIRE(wh::core::find_type_alias<registry_alpha, registry_beta>(
              wh::core::type_key{0U})
              .empty());
}
