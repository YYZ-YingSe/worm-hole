#include <catch2/catch_test_macros.hpp>

#include "wh/core/component/types.hpp"

namespace {

struct impl_specific_options {
  int limit{0};
};

} // namespace

TEST_CASE("component descriptors and common options preserve fields",
          "[UT][wh/core/component/types.hpp][component_descriptor][branch]") {
  const wh::core::component_descriptor descriptor{"worker", wh::core::component_kind::tool};
  const wh::core::component_common_options options{
      .callbacks_enabled = false,
      .trace_id = "trace-a",
      .span_id = "span-a",
  };

  REQUIRE(descriptor.type_name == "worker");
  REQUIRE(descriptor.kind == wh::core::component_kind::tool);
  REQUIRE_FALSE(options.callbacks_enabled);
  REQUIRE(options.trace_id == "trace-a");
  REQUIRE(options.span_id == "span-a");
}

TEST_CASE("component options resolve base call overrides and impl-specific data",
          "[UT][wh/core/component/types.hpp][component_options][branch][boundary]") {
  wh::core::component_options options{};
  options.set_base({.callbacks_enabled = true, .trace_id = "trace-a", .span_id = "span-a"});

  auto base_view = options.resolve_view();
  REQUIRE(base_view.callbacks_enabled);
  REQUIRE(base_view.trace_id == "trace-a");
  REQUIRE(base_view.span_id == "span-a");

  options.set_call_override({.callbacks_enabled = false,
                             .trace_id = std::string{"trace-b"},
                             .span_id = std::string{"span-b"}});
  const auto resolved = options.resolve();
  REQUIRE_FALSE(resolved.callbacks_enabled);
  REQUIRE(resolved.trace_id == "trace-b");
  REQUIRE(resolved.span_id == "span-b");

  REQUIRE(options.impl_specific_if<impl_specific_options>() == nullptr);
  REQUIRE(options.impl_specific_as<impl_specific_options>().has_error());

  options.set_impl_specific(impl_specific_options{7});
  REQUIRE(options.impl_specific_if<impl_specific_options>()->limit == 7);
  REQUIRE(options.impl_specific_as<impl_specific_options>().value().get().limit == 7);

  options.clear_call_override();
  REQUIRE(options.resolve().trace_id == "trace-a");
}

TEST_CASE("component options ignore absent overrides and keep base values",
          "[UT][wh/core/component/types.hpp][component_options::resolve_view][condition][branch]") {
  wh::core::component_options options{};
  options.set_base({.callbacks_enabled = false, .trace_id = "base-trace", .span_id = "base-span"});
  options.set_call_override(wh::core::component_override_options{});

  const auto resolved = options.resolve();
  const auto resolved_view = options.resolve_view();
  REQUIRE_FALSE(resolved.callbacks_enabled);
  REQUIRE(resolved.trace_id == "base-trace");
  REQUIRE(resolved.span_id == "base-span");
  REQUIRE_FALSE(resolved_view.callbacks_enabled);
  REQUIRE(resolved_view.trace_id == "base-trace");
  REQUIRE(resolved_view.span_id == "base-span");

  options.set_impl_specific(impl_specific_options{3});
  options.set_impl_specific(impl_specific_options{9});
  REQUIRE(options.impl_specific_if<impl_specific_options>()->limit == 9);
}
