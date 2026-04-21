#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/any.hpp"
#include "wh/core/json.hpp"
#include "wh/schema/serialization/registry.hpp"

TEST_CASE("serialization registry covers uniqueness lookup freeze pointer view and typed "
          "deserialize branches",
          "[UT][wh/schema/serialization/"
          "registry.hpp][serialization_registry::register_type][condition][branch][boundary]") {
  wh::schema::serialization_registry registry;
  registry.reserve(8U, 16U);

  auto empty_name = registry.register_type<std::int64_t>("");
  REQUIRE(empty_name.has_error());
  REQUIRE(empty_name.error() == wh::core::errc::invalid_argument);

  auto registered = registry.register_type<std::int64_t>("my.i64", {"legacy.i64"});
  REQUIRE(registered.has_value());
  REQUIRE(registry.size() == 1U);

  auto duplicate_type = registry.register_type<std::int64_t>("my.i64.v2");
  REQUIRE(duplicate_type.has_error());
  REQUIRE(duplicate_type.error() == wh::core::errc::already_exists);

  auto duplicate_name = registry.register_type<double>("my.i64");
  REQUIRE(duplicate_name.has_error());
  REQUIRE(duplicate_name.error() == wh::core::errc::already_exists);

  auto key = registry.key_for_name("legacy.i64");
  REQUIRE(key.has_value());
  REQUIRE(key.value() == wh::core::any_type_key_v<std::int64_t>);
  auto name = registry.primary_name_for_key(wh::core::any_type_key_v<std::int64_t>);
  REQUIRE(name.has_value());
  REQUIRE(name.value() == "my.i64");

  const std::int64_t input = 77;
  auto encoded = registry.serialize_view(wh::core::any_type_key_v<std::int64_t>, &input);
  REQUIRE(encoded.has_value());
  REQUIRE(encoded.value().IsInt64());

  auto any_encoded = registry.serialize_any(wh::core::any_type_key_v<std::int64_t>,
                                            wh::core::any{std::int64_t{88}});
  REQUIRE(any_encoded.has_value());

  auto bad_any = registry.serialize_any(wh::core::any_type_key_v<std::int64_t>,
                                        wh::core::any{std::string{"x"}});
  REQUIRE(bad_any.has_error());
  REQUIRE(bad_any.error() == wh::core::errc::type_mismatch);

  std::int64_t output = 0;
  auto decoded_status = registry.deserialize_to(
      "legacy.i64", wh::core::any_type_key_v<std::int64_t>, encoded.value(), &output);
  REQUIRE(decoded_status.has_value());
  REQUIRE(output == 77);

  auto decoded = registry.deserialize<std::int64_t>("legacy.i64", encoded.value());
  REQUIRE(decoded.has_value());
  REQUIRE(decoded.value() == 77);

  auto mismatch = registry.deserialize<std::string>("legacy.i64", encoded.value());
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);

  auto null_view = registry.serialize_view(wh::core::any_type_key_v<std::int64_t>, nullptr);
  REQUIRE(null_view.has_error());
  REQUIRE(null_view.error() == wh::core::errc::invalid_argument);

  auto null_decode = registry.deserialize_to("legacy.i64", wh::core::any_type_key_v<std::int64_t>,
                                             encoded.value(), nullptr);
  REQUIRE(null_decode.has_error());
  REQUIRE(null_decode.error() == wh::core::errc::invalid_argument);

  registry.freeze();
  REQUIRE(registry.is_frozen());
  auto frozen_register = registry.register_type<std::string>("my.string");
  REQUIRE(frozen_register.has_error());
  REQUIRE(frozen_register.error() == wh::core::errc::contract_violation);
}

TEST_CASE("serialization registry also covers deserialize_any and diagnostic alias registration",
          "[UT][wh/schema/serialization/"
          "registry.hpp][serialization_registry::deserialize_any][condition][branch][boundary]") {
  wh::schema::serialization_registry registry{};
  REQUIRE(registry.register_type_with_diagnostic_alias<int>().has_value());

  auto encoded = registry.serialize(5);
  REQUIRE(encoded.has_value());

  auto decoded_any =
      registry.deserialize_any(wh::internal::diagnostic_type_alias<int>(), encoded.value());
  REQUIRE(decoded_any.has_value());
  REQUIRE(*wh::core::any_cast<int>(&decoded_any.value()) == 5);

  auto missing = registry.deserialize_any("missing.type", encoded.value());
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  auto missing_key = registry.primary_name_for_key(wh::core::any_type_key_v<double>);
  REQUIRE(missing_key.has_error());
  REQUIRE(missing_key.error() == wh::core::errc::not_found);
}
