#include <functional>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/component.hpp"

namespace {

struct impl_specific_options {
  int limit{0};
};

struct descriptor_probe {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"descriptor-probe", wh::core::component_kind::custom};
  }
};

struct invokable_probe {
  [[nodiscard]] auto invoke(const int &value, wh::core::run_context &) -> wh::core::result<int> {
    return value + 1;
  }
};

struct streamable_probe {
  [[nodiscard]] auto stream(const int &value, wh::core::run_context &) -> wh::core::result<long> {
    return static_cast<long>(value * 2);
  }
};

using wh::core::component_common_options;
using wh::core::component_descriptor;
using wh::core::component_kind;
using wh::core::component_options;
using wh::core::component_override_options;

static_assert(wh::core::component_descriptor_provider<descriptor_probe>);
static_assert(wh::core::invokable_component<invokable_probe, int, int>);
static_assert(wh::core::streamable_component<streamable_probe, int, long>);

} // namespace

TEST_CASE("component descriptor stores type name and kind",
          "[UT][wh/core/component.hpp][component_descriptor][boundary]") {
  const component_descriptor descriptor{"worker", component_kind::tool};

  REQUIRE(descriptor.type_name == "worker");
  REQUIRE(descriptor.kind == component_kind::tool);
}

TEST_CASE("component options resolve base overrides and borrowed views",
          "[UT][wh/core/component.hpp][component_options::resolve][branch]") {
  component_options options{};
  options.set_base(component_common_options{
      .callbacks_enabled = true,
      .trace_id = "trace-a",
      .span_id = "span-a",
  });

  auto base_view = options.resolve_view();
  REQUIRE(base_view.callbacks_enabled);
  REQUIRE(base_view.trace_id == "trace-a");
  REQUIRE(base_view.span_id == "span-a");

  options.set_call_override(component_override_options{
      .callbacks_enabled = false,
      .trace_id = std::string{"trace-b"},
      .span_id = std::string{"span-b"},
  });

  auto resolved = options.resolve();
  auto resolved_view = options.resolve_view();
  REQUIRE_FALSE(resolved.callbacks_enabled);
  REQUIRE(resolved.trace_id == "trace-b");
  REQUIRE(resolved.span_id == "span-b");
  REQUIRE_FALSE(resolved_view.callbacks_enabled);
  REQUIRE(resolved_view.trace_id == "trace-b");
  REQUIRE(resolved_view.span_id == "span-b");

  options.clear_call_override();
  REQUIRE(options.resolve().trace_id == "trace-a");
  REQUIRE(options.call_override() == std::nullopt);
}

TEST_CASE("component options store and retrieve impl-specific payloads",
          "[UT][wh/core/component.hpp][component_options::impl_specific_as][branch][boundary]") {
  component_options options{};

  REQUIRE(options.impl_specific_if<impl_specific_options>() == nullptr);
  const auto missing = options.impl_specific_as<impl_specific_options>();
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  options.set_impl_specific(impl_specific_options{7});
  const auto *stored = options.impl_specific_if<impl_specific_options>();
  REQUIRE(stored != nullptr);
  REQUIRE(stored->limit == 7);

  const auto resolved = options.impl_specific_as<impl_specific_options>();
  REQUIRE(resolved.has_value());
  REQUIRE(resolved.value().get().limit == 7);

  options.set_impl_specific(impl_specific_options{11});
  REQUIRE(options.impl_specific_if<impl_specific_options>()->limit == 11);
}

TEST_CASE("component facade reexports descriptor invoke and stream concepts",
          "[UT][wh/core/component.hpp][component_descriptor_provider][condition]") {
  STATIC_REQUIRE(wh::core::component_descriptor_provider<descriptor_probe>);
  STATIC_REQUIRE(wh::core::invokable_component<invokable_probe, int, int>);
  STATIC_REQUIRE(wh::core::streamable_component<streamable_probe, int, long>);
  STATIC_REQUIRE(!wh::core::invokable_component<descriptor_probe, int, int>);
  STATIC_REQUIRE(!wh::core::streamable_component<invokable_probe, int, long>);
  SUCCEED();
}
