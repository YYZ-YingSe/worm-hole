#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include "wh/compose/callback.hpp"

TEST_CASE("compose callback adapter emits configured start end and error hooks",
          "[core][compose][callback][functional]") {
  std::size_t on_start_hits = 0U;
  std::size_t on_end_hits = 0U;
  std::size_t on_error_hits = 0U;

  wh::compose::callback_adapter callbacks{};
  callbacks.on_start = wh::core::callback_function<wh::core::result<void>(
      const wh::compose::graph_value &, wh::core::run_context &) const>{
      [&on_start_hits](const wh::compose::graph_value &,
                       wh::core::run_context &) -> wh::core::result<void> {
        ++on_start_hits;
        return {};
      }};
  callbacks.on_end = wh::core::callback_function<wh::core::result<void>(
      const wh::compose::graph_value &, wh::core::run_context &) const>{
      [&on_end_hits](const wh::compose::graph_value &,
                     wh::core::run_context &) -> wh::core::result<void> {
        ++on_end_hits;
        return {};
      }};
  callbacks.on_error = wh::core::callback_function<wh::core::result<void>(
      const wh::core::error_code, wh::core::run_context &) const>{
      [&on_error_hits](const wh::core::error_code,
                       wh::core::run_context &) -> wh::core::result<void> {
        ++on_error_hits;
        return {};
      }};

  wh::core::run_context context{};
  REQUIRE(callbacks.emit_start(wh::core::any(1), context).has_value());
  REQUIRE(callbacks.emit_end(wh::core::any(2), context).has_value());
  REQUIRE(callbacks.emit_error(wh::core::errc::internal_error, context).has_value());
  REQUIRE(on_start_hits == 1U);
  REQUIRE(on_end_hits == 1U);
  REQUIRE(on_error_hits == 1U);
}
