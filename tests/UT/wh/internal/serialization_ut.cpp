#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wh/internal/serialization.hpp"

namespace {

struct custom_codec_value {
  std::string text{};
};

auto wh_to_json(const custom_codec_value &input, wh::core::json_value &output,
                wh::core::json_allocator &allocator)
    -> wh::core::result<void> {
  output.SetObject();
  wh::core::json_value key{};
  key.SetString("text", allocator);
  wh::core::json_value value{};
  value.SetString(input.text.data(),
                  static_cast<wh::core::json_size_type>(input.text.size()),
                  allocator);
  output.AddMember(key.Move(), value.Move(), allocator);
  return {};
}

auto wh_from_json(const wh::core::json_value &input, custom_codec_value &output)
    -> wh::core::result<void> {
  const auto member = wh::core::json_find_member(input, "text");
  if (member.has_error()) {
    return wh::core::result<void>::failure(member.error());
  }
  if (!member.value()->IsString()) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }
  output.text = std::string{member.value()->GetString(),
                            member.value()->GetStringLength()};
  return {};
}

} // namespace

TEST_CASE("internal serialization encodes and decodes json object keys",
          "[UT][wh/internal/serialization.hpp][encode_json_key][branch][boundary]") {
  auto string_key = wh::internal::detail::encode_json_key(std::string{"name"});
  REQUIRE(string_key.has_value());
  REQUIRE(string_key.value() == "name");

  auto int_key = wh::internal::detail::encode_json_key(42);
  REQUIRE(int_key.has_value());
  REQUIRE(int_key.value() == "42");

  auto bool_key = wh::internal::detail::decode_json_key<bool>("true");
  REQUIRE(bool_key.has_value());
  REQUIRE(bool_key.value());

  auto decoded_int = wh::internal::detail::decode_json_key<int>("17");
  REQUIRE(decoded_int.has_value());
  REQUIRE(decoded_int.value() == 17);

  auto parse_error = wh::internal::detail::decode_json_key<int>("bad");
  REQUIRE(parse_error.has_error());
  REQUIRE(parse_error.error() == wh::core::errc::parse_error);
}

TEST_CASE("internal serialization handles custom codecs containers and pointers",
          "[UT][wh/internal/serialization.hpp][to_json_value][branch][boundary]") {
  STATIC_REQUIRE(wh::internal::detail::custom_json_codec<custom_codec_value>);

  wh::core::json_document document{};
  auto &allocator = document.GetAllocator();

  auto custom_json =
      wh::internal::to_json_value(custom_codec_value{.text = "hello"}, allocator);
  REQUIRE(custom_json.has_value());
  auto decoded_custom =
      wh::internal::from_json_value<custom_codec_value>(custom_json.value());
  REQUIRE(decoded_custom.has_value());
  REQUIRE(decoded_custom.value().text == "hello");

  auto vector_json = wh::internal::to_json_value(
      std::vector<int>{1, 2, 3}, allocator);
  REQUIRE(vector_json.has_value());
  auto decoded_vector =
      wh::internal::from_json_value<std::vector<int>>(vector_json.value());
  REQUIRE(decoded_vector.has_value());
  REQUIRE(decoded_vector.value() == std::vector<int>{1, 2, 3});

  auto map_json = wh::internal::to_json_value(
      std::map<std::string, int>{{"a", 1}, {"b", 2}}, allocator);
  REQUIRE(map_json.has_value());
  auto decoded_map =
      wh::internal::from_json_value<std::map<std::string, int>>(map_json.value());
  REQUIRE(decoded_map.has_value());
  REQUIRE(decoded_map.value().at("a") == 1);
  REQUIRE(decoded_map.value().at("b") == 2);

  auto unique_json = wh::internal::to_json_value(
      std::make_unique<int>(5), allocator);
  REQUIRE(unique_json.has_value());
  auto decoded_unique =
      wh::internal::from_json_value<std::unique_ptr<int>>(unique_json.value());
  REQUIRE(decoded_unique.has_value());
  REQUIRE(decoded_unique.value() != nullptr);
  REQUIRE(*decoded_unique.value() == 5);
}

TEST_CASE("internal serialization covers bool keys null pointers and unsupported key types",
          "[UT][wh/internal/serialization.hpp][decode_json_key][condition][branch][boundary]") {
  auto false_key = wh::internal::detail::encode_json_key(false);
  REQUIRE(false_key.has_value());
  REQUIRE(false_key.value() == "false");

  auto decoded_false = wh::internal::detail::decode_json_key<bool>("0");
  REQUIRE(decoded_false.has_value());
  REQUIRE_FALSE(decoded_false.value());

  auto unsupported =
      wh::internal::detail::encode_json_key(std::vector<int>{1, 2, 3});
  REQUIRE(unsupported.has_error());
  REQUIRE(unsupported.error() == wh::core::errc::not_supported);

  wh::core::json_document document{};
  auto &allocator = document.GetAllocator();

  auto null_shared =
      wh::internal::to_json_value(std::shared_ptr<int>{}, allocator);
  REQUIRE(null_shared.has_value());
  REQUIRE(null_shared.value().IsNull());

  auto decoded_shared =
      wh::internal::from_json_value<std::shared_ptr<int>>(null_shared.value());
  REQUIRE(decoded_shared.has_value());
  REQUIRE(decoded_shared.value() == nullptr);
}
