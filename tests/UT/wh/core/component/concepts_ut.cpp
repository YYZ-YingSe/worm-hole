#include <catch2/catch_test_macros.hpp>

#include "wh/core/component/concepts.hpp"

namespace {

struct descriptor_probe {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"descriptor-probe", wh::core::component_kind::custom};
  }
};

struct invoke_probe {
  [[nodiscard]] auto invoke(const int &value, wh::core::run_context &) -> wh::core::result<int> {
    return value + 1;
  }
};

struct stream_probe {
  [[nodiscard]] auto stream(const int &value, wh::core::run_context &) -> wh::core::result<long> {
    return static_cast<long>(value * 2);
  }
};

static_assert(wh::core::component_descriptor_provider<descriptor_probe>);
static_assert(wh::core::invokable_component<invoke_probe, int, int>);
static_assert(wh::core::streamable_component<stream_probe, int, long>);
static_assert(!wh::core::component_descriptor_provider<int>);

} // namespace

TEST_CASE("component concepts validate descriptor invoke and stream shapes",
          "[UT][wh/core/component/"
          "concepts.hpp][component_descriptor_provider][condition][branch][boundary]") {
  STATIC_REQUIRE(wh::core::component_descriptor_provider<descriptor_probe>);
  STATIC_REQUIRE_FALSE(wh::core::component_descriptor_provider<int>);

  const descriptor_probe descriptor{};
  const auto metadata = descriptor.descriptor();
  REQUIRE(metadata.type_name == "descriptor-probe");
  REQUIRE(metadata.kind == wh::core::component_kind::custom);
}

TEST_CASE("component concepts accept invokable and streamable component contracts",
          "[UT][wh/core/component/concepts.hpp][invokable_component][condition][branch]") {
  STATIC_REQUIRE(wh::core::invokable_component<invoke_probe, int, int>);
  STATIC_REQUIRE(wh::core::streamable_component<stream_probe, int, long>);

  wh::core::run_context context{};
  invoke_probe invokable{};
  auto invoked = invokable.invoke(3, context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value() == 4);

  stream_probe streamable{};
  auto streamed = streamable.stream(5, context);
  REQUIRE(streamed.has_value());
  REQUIRE(streamed.value() == 10);
}
