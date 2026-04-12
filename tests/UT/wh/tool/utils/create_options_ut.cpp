#include <catch2/catch_test_macros.hpp>

#include "wh/tool/utils/create_options.hpp"

TEST_CASE("create_options keeps base metadata and overlays call override values",
          "[UT][wh/tool/utils/create_options.hpp][create_options][branch][boundary]") {
  wh::tool::tool_options base{};
  base.set_base(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::retry,
      .max_retries = 1U,
      .timeout_label = "base",
  });

  wh::tool::tool_options call{};
  call.set_base(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::skip,
      .max_retries = 7U,
  });

  const auto merged = wh::tool::utils::create_options(base, call);
  const auto resolved = merged.resolve();
  REQUIRE(resolved.failure_policy == wh::tool::tool_failure_policy::skip);
  REQUIRE(resolved.max_retries == 7U);
  REQUIRE(resolved.timeout_label == "base");
}

TEST_CASE("create_options preserves base settings when call overlay stays empty",
          "[UT][wh/tool/utils/create_options.hpp][create_options][condition][boundary]") {
  wh::tool::tool_options base{};
  base.set_base(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::retry,
      .max_retries = 3U,
      .timeout_label = "base",
  });

  wh::tool::tool_options call{};
  call.set_base(wh::tool::tool_common_options{});

  const auto merged = wh::tool::utils::create_options(base, call);
  const auto resolved = merged.resolve();
  REQUIRE(resolved.failure_policy == wh::tool::tool_failure_policy::fail_fast);
  REQUIRE(resolved.max_retries == 0U);
  REQUIRE(resolved.timeout_label == "base");
}
