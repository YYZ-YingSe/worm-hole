#include <catch2/catch_test_macros.hpp>

#include "wh/tool/options.hpp"

namespace {

struct tool_options_probe {
  int value{0};
};

} // namespace

TEST_CASE("tool options resolve base and override fields",
          "[UT][wh/tool/options.hpp][tool_options::resolve][branch][boundary]") {
  wh::tool::tool_options options{};
  wh::tool::tool_common_options base{};
  base.failure_policy = wh::tool::tool_failure_policy::retry;
  base.max_retries = 2U;
  base.timeout_label = "base";
  options.set_base(base);

  wh::tool::tool_common_options override{};
  override.failure_policy = wh::tool::tool_failure_policy::skip;
  override.max_retries = 8U;
  options.set_call_override(std::move(override));

  const auto view = options.resolve_view();
  REQUIRE(view.failure_policy == wh::tool::tool_failure_policy::skip);
  REQUIRE(view.max_retries == 8U);
  REQUIRE(view.timeout_label == "base");

  const auto resolved = options.resolve();
  REQUIRE(resolved.failure_policy == wh::tool::tool_failure_policy::skip);
  REQUIRE(resolved.max_retries == 8U);
  REQUIRE(resolved.timeout_label == "base");
}

TEST_CASE("tool options expose component specific extras",
          "[UT][wh/tool/options.hpp][tool_options::component_options][boundary]") {
  wh::tool::tool_options options{};
  options.component_options().set_impl_specific(tool_options_probe{21});
  const auto *probe =
      options.component_options().impl_specific_if<tool_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 21);
}

TEST_CASE("tool options preserve base timeout label when override leaves it empty",
          "[UT][wh/tool/options.hpp][tool_options::resolve_view][condition][boundary]") {
  wh::tool::tool_options options{};
  options.set_base(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::retry,
      .max_retries = 2U,
      .timeout_label = "base",
  });
  options.set_call_override(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::skip,
      .max_retries = 1U,
      .timeout_label = "",
  });

  const auto view = options.resolve_view();
  REQUIRE(view.failure_policy == wh::tool::tool_failure_policy::skip);
  REQUIRE(view.max_retries == 1U);
  REQUIRE(view.timeout_label == "base");
  REQUIRE(options.call_override().has_value());
  REQUIRE(options.base().timeout_label == "base");
}
