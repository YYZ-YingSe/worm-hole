#include <catch2/catch_test_macros.hpp>

#include "wh/prompt/options.hpp"

namespace {

struct prompt_options_probe {
  int value{0};
};

} // namespace

TEST_CASE("prompt options resolve base and override while preserving base name when empty",
          "[UT][wh/prompt/options.hpp][prompt_options::resolve][branch][boundary]") {
  wh::prompt::prompt_options options{};
  wh::prompt::prompt_common_options base{};
  base.syntax = wh::prompt::template_syntax::jinja_compatible;
  base.strict_missing_variables = true;
  base.template_name = "base-template";
  options.set_base(base);

  wh::prompt::prompt_common_options override{};
  override.syntax = wh::prompt::template_syntax::placeholder;
  override.strict_missing_variables = false;
  override.template_name.clear();
  options.set_call_override(std::move(override));

  const auto view = options.resolve_view();
  REQUIRE(view.syntax == wh::prompt::template_syntax::placeholder);
  REQUIRE_FALSE(view.strict_missing_variables);
  REQUIRE(view.template_name == "base-template");

  const auto resolved = options.resolve();
  REQUIRE(resolved.template_name == "base-template");
  REQUIRE_FALSE(resolved.strict_missing_variables);
}

TEST_CASE("prompt options expose component specific extras",
          "[UT][wh/prompt/options.hpp][prompt_options::component_options]") {
  wh::prompt::prompt_options options{};
  options.component_options().set_impl_specific(prompt_options_probe{17});
  const auto *probe = options.component_options().impl_specific_if<prompt_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 17);
}

TEST_CASE(
    "prompt options keep base view without override and support direct impl-specific access",
    "[UT][wh/prompt/options.hpp][prompt_options::set_impl_specific][condition][branch][boundary]") {
  wh::prompt::prompt_options options{};
  options.set_base(
      wh::prompt::prompt_common_options{.syntax = wh::prompt::template_syntax::placeholder,
                                        .strict_missing_variables = true,
                                        .template_name = "base-only"});
  options.set_impl_specific(prompt_options_probe{23});

  const auto view = options.resolve_view();
  REQUIRE(view.syntax == wh::prompt::template_syntax::placeholder);
  REQUIRE(view.strict_missing_variables);
  REQUIRE(view.template_name == "base-only");

  const auto *probe = options.impl_specific_if<prompt_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 23);
}
