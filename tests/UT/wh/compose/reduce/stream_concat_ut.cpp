#include <catch2/catch_test_macros.hpp>

#include "wh/compose/reduce/stream_concat.hpp"

TEST_CASE("compose stream concat delegates typed and dynamic concat paths",
          "[UT][wh/compose/reduce/stream_concat.hpp][stream_concat][condition][branch][boundary]") {
  wh::internal::stream_concat_registry registry{};
  registry.reserve(4U);
  REQUIRE(registry.size() == 0U);
  REQUIRE_FALSE(registry.is_frozen());
  REQUIRE(registry
              .register_concat<std::string>([](std::span<const std::string> values)
                                                -> wh::core::result<std::string> {
                std::string joined{};
                for (const auto &value : values) {
                  joined += value;
                }
                return joined;
              })
              .has_value());

  const std::array typed_values = {std::string{"a"}, std::string{"b"}};
  auto typed =
      wh::compose::stream_concat(registry, std::span<const std::string>{typed_values});
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == "ab");

  const std::array dynamic_values = {wh::core::any{std::string{"a"}},
                                     wh::core::any{std::string{"c"}}};
  auto dynamic = wh::compose::stream_concat(
      registry, wh::core::any_type_key_v<std::string>, dynamic_values);
  REQUIRE(dynamic.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&dynamic.value()) == "ac");

  registry.freeze();
  REQUIRE(registry.is_frozen());
}

TEST_CASE("compose stream concat surfaces empty unsupported and type mismatch branches",
          "[UT][wh/compose/reduce/stream_concat.hpp][stream_concat][condition][branch][boundary]") {
  wh::internal::stream_concat_registry registry{};

  auto empty = wh::compose::stream_concat(registry, std::span<const std::string>{});
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::errc::invalid_argument);

  const std::array singleton = {std::string{"only"}};
  auto single = wh::compose::stream_concat(registry, std::span<const std::string>{singleton});
  REQUIRE(single.has_value());
  REQUIRE(single.value() == "only");

  const std::array mismatch_values = {wh::core::any{std::string{"x"}}};
  auto mismatch = wh::compose::stream_concat(
      registry, wh::core::any_type_key_v<int>, mismatch_values);
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);

  const std::array unsupported_values = {wh::core::any{1}, wh::core::any{2}};
  auto unsupported = wh::compose::stream_concat(
      registry, wh::core::any_type_key_v<int>, unsupported_values);
  REQUIRE(unsupported.has_error());
  REQUIRE(unsupported.error() == wh::core::errc::not_supported);
}
