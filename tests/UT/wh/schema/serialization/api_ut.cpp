#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wh/core/json.hpp"
#include "wh/schema/serialization/api.hpp"

namespace {

struct custom_point {
  int x{};
  int y{};
};

auto wh_to_json(const custom_point &input, wh::core::json_value &output,
                wh::core::json_allocator &allocator) -> wh::core::result<void> {
  output.SetObject();
  wh::core::json_value x_name;
  x_name.SetString("x", allocator);
  wh::core::json_value x_value;
  x_value.SetInt(input.x);
  output.AddMember(x_name.Move(), x_value.Move(), allocator);
  wh::core::json_value y_name;
  y_name.SetString("y", allocator);
  wh::core::json_value y_value;
  y_value.SetInt(input.y);
  output.AddMember(y_name.Move(), y_value.Move(), allocator);
  return {};
}

auto wh_from_json(const wh::core::json_value &input, custom_point &output)
    -> wh::core::result<void> {
  if (!input.IsObject()) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }
  const auto x = input.FindMember("x");
  const auto y = input.FindMember("y");
  if (x == input.MemberEnd() || y == input.MemberEnd() ||
      !x->value.IsInt() || !y->value.IsInt()) {
    return wh::core::result<void>::failure(wh::core::errc::parse_error);
  }
  output.x = x->value.GetInt();
  output.y = y->value.GetInt();
  return {};
}

} // namespace

TEST_CASE("serialization api fast path and default registry builders cover encode decode and registry preload",
          "[UT][wh/schema/serialization/api.hpp][make_default_serialization_registry][branch][boundary]") {
  const custom_point point{7, 11};
  const auto encoded = wh::schema::serialize_fast(point);
  REQUIRE(encoded.has_value());
  REQUIRE(encoded.value().IsObject());

  const auto decoded =
      wh::schema::deserialize_fast<custom_point>(encoded.value());
  REQUIRE(decoded.has_value());
  REQUIRE(decoded.value().x == 7);
  REQUIRE(decoded.value().y == 11);

  wh::core::json_document doc;
  const auto to_status = wh::schema::serialize_fast_to(point, doc);
  REQUIRE(to_status.has_value());
  REQUIRE(doc.IsObject());

  const auto mismatch =
      wh::schema::deserialize_fast<std::int64_t>(encoded.value());
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);

  auto registry = wh::schema::make_default_serialization_registry();
  REQUIRE(registry.has_value());
  REQUIRE(registry.value().size() >= 7U);
  REQUIRE(registry.value().key_for_name("string").has_value());
  REQUIRE(registry.value().key_for_name("std.string").has_value());
}

TEST_CASE("serialization api rejects duplicate default-type preload on the same registry",
          "[UT][wh/schema/serialization/api.hpp][register_default_types][condition][branch]") {
  wh::schema::serialization_registry registry{};

  auto first = wh::schema::register_default_types(registry);
  REQUIRE(first.has_value());
  REQUIRE(registry.key_for_name("text").has_value());
  REQUIRE(registry.key_for_name("map.string.string").has_value());

  auto duplicate = wh::schema::register_default_types(registry);
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);
}
