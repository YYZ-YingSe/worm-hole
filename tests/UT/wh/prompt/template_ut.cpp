#include <catch2/catch_test_macros.hpp>

#include "wh/prompt/template.hpp"

TEST_CASE("render text template resolves placeholder paths and reports missing bindings",
          "[UT][wh/prompt/template.hpp][render_text_template][branch][boundary]") {
  wh::prompt::template_context context{
      {"user", wh::prompt::template_object{{"name", "Ada"}}},
      {"items", wh::prompt::template_array{wh::prompt::template_value{"first"},
                                           wh::prompt::template_value{"second"}}},
  };

  auto rendered = wh::prompt::render_text_template("Hello {{ user.name }} {{ items.1 }}", context);
  REQUIRE(rendered.has_value());
  REQUIRE(rendered.value() == "Hello Ada second");

  auto missing = wh::prompt::render_text_template("{{ missing }}", context);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("render text template supports jinja mode and surfaces parse failures",
          "[UT][wh/prompt/template.hpp][render_text_template][branch]") {
  wh::prompt::template_context context{
      {"name", "WormHole"},
  };

  auto rendered = wh::prompt::render_text_template("Hello {{ name }}", context,
                                                   wh::prompt::template_syntax::jinja_compatible);
  REQUIRE(rendered.has_value());
  REQUIRE(rendered.value() == "Hello WormHole");

  auto broken = wh::prompt::render_text_template("{% if name %}", context,
                                                 wh::prompt::template_syntax::jinja_compatible);
  REQUIRE(broken.has_error());
  REQUIRE(broken.error() == wh::core::errc::parse_error);
}

TEST_CASE(
    "template detail helpers parse indexes map runtime errors and reject malformed placeholders",
    "[UT][wh/prompt/template.hpp][detail::parse_index][condition][branch][boundary]") {
  auto index = wh::prompt::detail::parse_index("12");
  REQUIRE(index.has_value());
  REQUIRE(index.value() == 12U);

  auto bad_index = wh::prompt::detail::parse_index("1x");
  REQUIRE(bad_index.has_error());
  REQUIRE(bad_index.error() == wh::core::errc::type_mismatch);

  REQUIRE(wh::prompt::detail::map_minja_runtime_error("Undefined variable foo") ==
          wh::core::errc::not_found);
  REQUIRE(wh::prompt::detail::map_minja_runtime_error("Unknown filter bar") ==
          wh::core::errc::not_supported);
  REQUIRE(wh::prompt::detail::map_minja_runtime_error("object is not iterable") ==
          wh::core::errc::type_mismatch);

  wh::prompt::template_context context{{"name", "Ada"}};
  auto malformed = wh::prompt::detail::placeholder_render("Hello {{ name", context);
  REQUIRE(malformed.has_error());
  REQUIRE(malformed.error() == wh::core::errc::parse_error);
}
