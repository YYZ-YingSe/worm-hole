#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/json.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/internal/safe.hpp"
#include "wh/internal/serialization.hpp"
#include "wh/schema/serialization.hpp"
#include "wh/schema/serialization_registry.hpp"

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

  const auto x_member = input.FindMember("x");
  const auto y_member = input.FindMember("y");
  if (x_member == input.MemberEnd() || y_member == input.MemberEnd() ||
      !x_member->value.IsInt() || !y_member->value.IsInt()) {
    return wh::core::result<void>::failure(wh::core::errc::parse_error);
  }

  output.x = x_member->value.GetInt();
  output.y = y_member->value.GetInt();
  return {};
}

struct encode_only_point {
  int value{};
};

[[maybe_unused]] auto wh_to_json(const encode_only_point &input,
                                 wh::core::json_value &output,
                                 wh::core::json_allocator &)
    -> wh::core::result<void> {
  output.SetInt(input.value);
  return {};
}

struct tracked_raw_pointer_value {
  static inline int live_count = 0;

  int value{};

  tracked_raw_pointer_value() {
    ++live_count;
  }

  ~tracked_raw_pointer_value() {
    --live_count;
  }

  tracked_raw_pointer_value(const tracked_raw_pointer_value &) = delete;
  auto operator=(const tracked_raw_pointer_value &)
      -> tracked_raw_pointer_value & = delete;
  tracked_raw_pointer_value(tracked_raw_pointer_value &&) = delete;
  auto operator=(tracked_raw_pointer_value &&)
      -> tracked_raw_pointer_value & = delete;
};

[[maybe_unused]] auto wh_to_json(const tracked_raw_pointer_value &input,
                                 wh::core::json_value &output,
                                 wh::core::json_allocator &)
    -> wh::core::result<void> {
  output.SetInt(input.value);
  return {};
}

auto wh_from_json(const wh::core::json_value &input,
                  tracked_raw_pointer_value &output) -> wh::core::result<void> {
  if (!input.IsInt()) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }
  output.value = input.GetInt();
  return {};
}

} // namespace

TEST_CASE("json parsing helpers provide typed access and structured errors",
          "[core][json][condition]") {
  const auto parsed = wh::core::parse_json(
      R"({"flag":true,"items":[1,2,3],"name":"worm-hole"})");
  REQUIRE(parsed.has_value());
  REQUIRE(wh::core::json_kind(parsed.value()) == wh::core::json_value_kind::object_value);

  const auto items_member = wh::core::json_find_member(parsed.value(), "items");
  REQUIRE(items_member.has_value());
  REQUIRE(items_member.value() != nullptr);
  REQUIRE(items_member.value()->IsArray());

  const auto array_item = wh::core::json_at(*items_member.value(), 1U);
  REQUIRE(array_item.has_value());
  REQUIRE(array_item.value() != nullptr);
  REQUIRE(array_item.value()->GetInt() == 2);

  const auto missing_member = wh::core::json_find_member(parsed.value(), "missing");
  REQUIRE(missing_member.has_error());
  REQUIRE(missing_member.error() == wh::core::errc::not_found);

  const auto wrong_type_at = wh::core::json_at(parsed.value(), 0U);
  REQUIRE(wrong_type_at.has_error());
  REQUIRE(wrong_type_at.error() == wh::core::errc::type_mismatch);

  const auto parse_failed = wh::core::parse_json("{]");
  REQUIRE(parse_failed.has_error());
  REQUIRE(parse_failed.error() == wh::core::errc::parse_error);

  const auto detailed_failed = wh::core::parse_json_with_error("{]");
  REQUIRE(detailed_failed.has_error());
  REQUIRE(detailed_failed.error().code != rapidjson::kParseErrorNone);
  REQUIRE_FALSE(wh::core::parse_error_message(detailed_failed.error()).empty());
}

TEST_CASE("serialization handles containers pointers and codec constraints",
          "[core][serialization][branch]") {
  wh::core::json_document arena;

  const std::map<int, std::string> keyed_map{{1, "a"}, {2, "b"}};
  const auto encoded_map = wh::internal::to_json_value(keyed_map, arena.GetAllocator());
  REQUIRE(encoded_map.has_value());
  REQUIRE(encoded_map.value().IsObject());

  const auto decoded_map =
      wh::internal::from_json_value<std::map<int, std::string>>(encoded_map.value());
  REQUIRE(decoded_map.has_value());
  REQUIRE(decoded_map.value() == keyed_map);

  const std::optional<int> maybe_value{42};
  const auto encoded_optional =
      wh::internal::to_json_value(maybe_value, arena.GetAllocator());
  REQUIRE(encoded_optional.has_value());

  const auto decoded_optional =
      wh::internal::from_json_value<std::optional<int>>(encoded_optional.value());
  REQUIRE(decoded_optional.has_value());
  REQUIRE(decoded_optional.value().has_value());
  REQUIRE(*decoded_optional.value() == 42);

  const std::unique_ptr<int> owned_value = std::make_unique<int>(7);
  const auto encoded_ptr =
      wh::internal::to_json_value(owned_value, arena.GetAllocator());
  REQUIRE(encoded_ptr.has_value());

  const auto decoded_ptr =
      wh::internal::from_json_value<std::unique_ptr<int>>(encoded_ptr.value());
  REQUIRE(decoded_ptr.has_value());
  REQUIRE(decoded_ptr.value() != nullptr);
  REQUIRE(*decoded_ptr.value() == 7);

  wh::core::json_document oversized_array;
  oversized_array.Parse("[1,2,3]");
  std::array<int, 2U> fixed_array{};
  const auto decoded_array = wh::internal::from_json(oversized_array, fixed_array);
  REQUIRE(decoded_array.has_error());
  REQUIRE(decoded_array.error() == wh::core::errc::parse_error);

  const custom_point point{3, 9};
  const auto encoded_point =
      wh::internal::to_json_value(point, arena.GetAllocator());
  REQUIRE(encoded_point.has_value());
  const auto decoded_point =
      wh::internal::from_json_value<custom_point>(encoded_point.value());
  REQUIRE(decoded_point.has_value());
  REQUIRE(decoded_point.value().x == 3);
  REQUIRE(decoded_point.value().y == 9);

  const encode_only_point one_way{5};
  const auto encoded_one_way =
      wh::internal::to_json_value(one_way, arena.GetAllocator());
  REQUIRE(encoded_one_way.has_error());
  REQUIRE(encoded_one_way.error() == wh::core::errc::not_supported);
}

TEST_CASE("raw pointer deserialization releases previous allocation",
          "[core][serialization][boundary]") {
  tracked_raw_pointer_value::live_count = 0;

  wh::core::json_document first_input;
  first_input.Parse("1");
  tracked_raw_pointer_value *output = nullptr;
  const auto first_status = wh::internal::from_json(first_input, output);
  REQUIRE(first_status.has_value());
  REQUIRE(output != nullptr);
  REQUIRE(output->value == 1);
  REQUIRE(tracked_raw_pointer_value::live_count == 1);

  wh::core::json_document second_input;
  second_input.Parse("2");
  const auto second_status = wh::internal::from_json(second_input, output);
  REQUIRE(second_status.has_value());
  REQUIRE(output != nullptr);
  REQUIRE(output->value == 2);
  REQUIRE(tracked_raw_pointer_value::live_count == 1);

  wh::core::json_document null_input;
  null_input.Parse("null");
  const auto null_status = wh::internal::from_json(null_input, output);
  REQUIRE(null_status.has_value());
  REQUIRE(output == nullptr);
  REQUIRE(tracked_raw_pointer_value::live_count == 0);
}

TEST_CASE("serialization registry keeps name type uniqueness and alias routing",
          "[core][serialization_registry][condition]") {
  wh::schema::serialization_registry registry;

  const auto registered =
      registry.register_type<std::int64_t>("my.i64", {"legacy.i64"});
  REQUIRE(registered.has_value());
  REQUIRE(registry.size() == 1U);

  const auto duplicate_type = registry.register_type<std::int64_t>("my.i64.v2");
  REQUIRE(duplicate_type.has_error());
  REQUIRE(duplicate_type.error() == wh::core::errc::already_exists);

  const auto duplicate_name = registry.register_type<double>("my.i64");
  REQUIRE(duplicate_name.has_error());
  REQUIRE(duplicate_name.error() == wh::core::errc::already_exists);

  const auto type_lookup = registry.type_for_name("legacy.i64");
  REQUIRE(type_lookup.has_value());
  REQUIRE(type_lookup.value() == std::type_index(typeid(std::int64_t)));

  const auto primary_name =
      registry.primary_name_for_type(std::type_index(typeid(std::int64_t)));
  REQUIRE(primary_name.has_value());
  REQUIRE(primary_name.value() == "my.i64");

  const auto encoded = registry.serialize<std::int64_t>(88);
  REQUIRE(encoded.has_value());
  REQUIRE(encoded.value().IsInt64());

  const auto decoded =
      registry.deserialize<std::int64_t>("legacy.i64", encoded.value());
  REQUIRE(decoded.has_value());
  REQUIRE(decoded.value() == 88);

  const auto mismatch =
      registry.deserialize<std::string>("legacy.i64", encoded.value());
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);

  const auto missing_type = registry.type_for_name("missing");
  REQUIRE(missing_type.has_error());
  REQUIRE(missing_type.error() == wh::core::errc::not_found);
}

TEST_CASE("serialization registry freeze blocks late registration",
          "[core][serialization_registry][condition]") {
  wh::schema::serialization_registry registry;
  registry.reserve(8U, 16U);

  const auto registered = registry.register_type<std::int64_t>("my.i64");
  REQUIRE(registered.has_value());
  REQUIRE_FALSE(registry.is_frozen());

  registry.freeze();
  REQUIRE(registry.is_frozen());

  const auto late_register = registry.register_type<std::string>("my.string");
  REQUIRE(late_register.has_error());
  REQUIRE(late_register.error() == wh::core::errc::contract_violation);

  const std::int64_t value = 123;
  const auto encoded = registry.serialize(value);
  REQUIRE(encoded.has_value());
  REQUIRE(encoded.value().IsInt64());
}

TEST_CASE("serialization registry pointer view api keeps dynamic path typed",
          "[core][serialization_registry][condition]") {
  wh::schema::serialization_registry registry;
  const auto registered = registry.register_type<std::int64_t>(
      "my.i64", {"legacy.i64"});
  REQUIRE(registered.has_value());

  const std::int64_t input = 77;
  const auto encoded =
      registry.serialize_view(std::type_index(typeid(std::int64_t)), &input);
  REQUIRE(encoded.has_value());
  REQUIRE(encoded.value().IsInt64());
  REQUIRE(encoded.value().GetInt64() == 77);

  std::int64_t output = 0;
  const auto decoded_status = registry.deserialize_to(
      "legacy.i64", std::type_index(typeid(std::int64_t)), encoded.value(),
      &output);
  REQUIRE(decoded_status.has_value());
  REQUIRE(output == 77);

  const auto mismatch_status =
      registry.deserialize_to("legacy.i64", std::type_index(typeid(double)),
                              encoded.value(), &output);
  REQUIRE(mismatch_status.has_error());
  REQUIRE(mismatch_status.error() == wh::core::errc::type_mismatch);

  const auto null_serialize =
      registry.serialize_view(std::type_index(typeid(std::int64_t)), nullptr);
  REQUIRE(null_serialize.has_error());
  REQUIRE(null_serialize.error() == wh::core::errc::invalid_argument);

  const auto null_deserialize = registry.deserialize_to(
      "legacy.i64", std::type_index(typeid(std::int64_t)), encoded.value(),
      nullptr);
  REQUIRE(null_deserialize.has_error());
  REQUIRE(null_deserialize.error() == wh::core::errc::invalid_argument);
}

TEST_CASE("serialization fast path bypasses registry lookup",
          "[core][serialization][hot_path]") {
  const custom_point point{7, 11};

  const auto encoded = wh::schema::serialize_fast(point);
  REQUIRE(encoded.has_value());
  REQUIRE(encoded.value().IsObject());

  const auto decoded = wh::schema::deserialize_fast<custom_point>(encoded.value());
  REQUIRE(decoded.has_value());
  REQUIRE(decoded.value().x == 7);
  REQUIRE(decoded.value().y == 11);

  wh::schema::serialization_registry registry;
  const auto dynamic_lookup = registry.serialize(point);
  REQUIRE(dynamic_lookup.has_error());
  REQUIRE(dynamic_lookup.error() == wh::core::errc::not_found);

  const auto mismatch = wh::schema::deserialize_fast<std::int64_t>(encoded.value());
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("default schema registration and error domain mapping contracts",
          "[core][error_domain][boundary]") {
  const auto default_registry = wh::schema::make_default_serialization_registry();
  REQUIRE(default_registry.has_value());
  REQUIRE(default_registry.value().size() >= 7U);
  REQUIRE(default_registry.value().type_for_name("string").has_value());
  REQUIRE(default_registry.value().type_for_name("std.string").has_value());

  REQUIRE(wh::core::map_exception(std::bad_alloc{}) ==
          wh::core::errc::resource_exhausted);
  REQUIRE(wh::core::map_exception(std::invalid_argument{"bad"}) ==
          wh::core::errc::invalid_argument);
  REQUIRE(wh::core::map_exception(std::runtime_error{"boom"}) ==
          wh::core::errc::internal_error);

  const auto exception_mapped =
      wh::core::exception_boundary<int>([]() -> int {
        throw std::invalid_argument{"bad"};
      });
  REQUIRE(exception_mapped.has_error());
  REQUIRE(exception_mapped.error() == wh::core::errc::invalid_argument);

  const auto safe_fallback = wh::internal::safe_call<int>(
      []() -> int { throw std::runtime_error{"boom"}; },
      wh::core::errc::network_error);
  REQUIRE(safe_fallback.has_error());
  REQUIRE(safe_fallback.error() == wh::core::errc::network_error);

  const auto safe_oom = wh::internal::safe_call<int>([]() -> int {
    throw std::bad_alloc{};
  });
  REQUIRE(safe_oom.has_error());
  REQUIRE(safe_oom.error() == wh::core::errc::resource_exhausted);
}
