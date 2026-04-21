#include <catch2/catch_test_macros.hpp>

#include "wh/compose/payload/keyed.hpp"

TEST_CASE("keyed payload helpers pack unpack and project keyed inputs",
          "[UT][wh/compose/payload/keyed.hpp][pack_keyed_payload][condition][branch][boundary]") {
  auto payload = wh::compose::pack_keyed_payload("answer", wh::compose::graph_value{42});
  auto unpacked = wh::compose::unpack_keyed_payload(payload, "answer");
  REQUIRE(unpacked.has_value());
  REQUIRE(*wh::core::any_cast<int>(&unpacked.value()) == 42);

  auto output = wh::compose::keyed_output("text", std::string{"hello"});
  auto projected = wh::compose::keyed_input(output, "text");
  REQUIRE(projected.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&projected.value()) == "hello");

  auto moved_projected = wh::compose::keyed_input(std::move(output), "text");
  REQUIRE(moved_projected.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&moved_projected.value()) == "hello");

  auto missing = wh::compose::unpack_keyed_payload(payload, "missing");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  auto invalid = wh::compose::keyed_input(wh::compose::graph_value{7}, "text");
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("keyed payload helpers preserve movable payloads across map wrappers",
          "[UT][wh/compose/payload/keyed.hpp][keyed_output][condition][branch][boundary]") {
  auto packed =
      wh::compose::pack_keyed_payload("unique", wh::compose::graph_value{std::string{"only"}});
  auto unpacked = wh::compose::unpack_keyed_payload(std::move(packed), "unique");
  REQUIRE(unpacked.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&unpacked.value()) == "only");

  auto keyed = wh::compose::keyed_output("number", 11);
  auto extracted = wh::compose::unpack_keyed_payload(std::move(keyed), "number");
  REQUIRE(extracted.has_value());
  REQUIRE(*wh::core::any_cast<int>(&extracted.value()) == 11);
}
