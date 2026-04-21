#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/serialization.hpp"

TEST_CASE("serialization facade exposes fast codecs and default registry",
          "[UT][wh/schema/"
          "serialization.hpp][make_default_serialization_registry][condition][branch][boundary]") {
  const auto encoded = wh::schema::serialize_fast(std::string{"alpha"});
  REQUIRE(encoded.has_value());

  const auto decoded = wh::schema::deserialize_fast<std::string>(encoded.value());
  REQUIRE(decoded.has_value());
  REQUIRE(decoded.value() == "alpha");

  auto registry = wh::schema::make_default_serialization_registry();
  REQUIRE(registry.has_value());
  REQUIRE(registry.value().size() >= 7U);
  REQUIRE(registry.value().key_for_name("string").has_value());
}

TEST_CASE("serialization facade round-trips primitive values through fast helpers",
          "[UT][wh/schema/serialization.hpp][serialize_fast][condition][branch][boundary]") {
  const auto encoded = wh::schema::serialize_fast(std::int64_t{42});
  REQUIRE(encoded.has_value());

  const auto decoded = wh::schema::deserialize_fast<std::int64_t>(encoded.value());
  REQUIRE(decoded.has_value());
  REQUIRE(decoded.value() == 42);
}
