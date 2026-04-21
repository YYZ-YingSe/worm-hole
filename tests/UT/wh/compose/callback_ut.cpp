#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/callback.hpp"

static_assert(std::is_default_constructible_v<wh::compose::callback_adapter>);

TEST_CASE("compose callback facade exports callback_adapter through the public header",
          "[UT][wh/compose/callback.hpp][callback_adapter][condition][branch][boundary]") {
  wh::compose::callback_adapter adapter{};

  REQUIRE_FALSE(static_cast<bool>(adapter.on_start));
  REQUIRE_FALSE(static_cast<bool>(adapter.on_end));
  REQUIRE_FALSE(static_cast<bool>(adapter.on_error));
}

TEST_CASE(
    "compose callback facade allows configuring and invoking public adapter hooks",
    "[UT][wh/compose/callback.hpp][callback_adapter::emit_start][condition][branch][boundary]") {
  wh::compose::callback_adapter adapter{};
  wh::compose::graph_value input{7};
  wh::core::run_context context{};
  int stages = 0;

  adapter.on_start = [&stages](const wh::compose::graph_value &,
                               wh::core::run_context &) -> wh::core::result<void> {
    ++stages;
    return {};
  };
  adapter.on_end = [&stages](const wh::compose::graph_value &,
                             wh::core::run_context &) -> wh::core::result<void> {
    stages += 10;
    return {};
  };

  REQUIRE(adapter.emit_start(input, context).has_value());
  REQUIRE(adapter.emit_end(input, context).has_value());
  REQUIRE(stages == 11);
}
