#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <vector>

#include "wh/callbacks/callback_builder.hpp"

namespace {

using stage_callback_like_t = decltype([](wh::callbacks::stage,
                                          wh::callbacks::event_view,
                                          const wh::callbacks::run_info &) {});
using not_stage_callback_like_t = decltype([](int) {});

static_assert(
    wh::callbacks::detail::StageCallbackLike<stage_callback_like_t>);
static_assert(
    !wh::callbacks::detail::StageCallbackLike<not_stage_callback_like_t>);

auto make_info() -> wh::callbacks::run_info {
  wh::callbacks::run_info info{};
  info.name = "builder";
  info.type = "builder";
  return info;
}

} // namespace

TEST_CASE("stage callback builder reports empty before any stage callback is set",
          "[UT][wh/callbacks/callback_builder.hpp][stage_callback_builder::empty][condition][branch][boundary]") {
  wh::callbacks::stage_callback_builder builder{};

  REQUIRE(builder.empty());
}

TEST_CASE("stage callback builder rejects build when no stage callback exists",
          "[UT][wh/callbacks/callback_builder.hpp][stage_callback_builder::build_callbacks][condition][branch][boundary]") {
  wh::callbacks::stage_callback_builder builder{};

  const auto missing = builder.build_callbacks();

  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("stage callback builder installs every stage hook and reset clears them",
          "[UT][wh/callbacks/callback_builder.hpp][stage_callback_builder::reset][condition][branch][boundary]") {
  wh::callbacks::stage_callback_builder builder{};
  std::vector<wh::callbacks::stage> seen{};

  builder.on_start([&seen](const wh::callbacks::stage stage,
                           const wh::callbacks::event_view,
                           const wh::callbacks::run_info &) {
    seen.push_back(stage);
  });
  REQUIRE_FALSE(builder.empty());

  auto built =
      builder
          .on_end([&seen](const wh::callbacks::stage stage,
                          const wh::callbacks::event_view,
                          const wh::callbacks::run_info &) {
            seen.push_back(stage);
          })
          .on_error([&seen](const wh::callbacks::stage stage,
                            const wh::callbacks::event_view,
                            const wh::callbacks::run_info &) {
            seen.push_back(stage);
          })
          .on_stream_start([&seen](const wh::callbacks::stage stage,
                                   const wh::callbacks::event_view,
                                   const wh::callbacks::run_info &) {
            seen.push_back(stage);
          })
          .on_stream_end([&seen](const wh::callbacks::stage stage,
                                 const wh::callbacks::event_view,
                                 const wh::callbacks::run_info &) {
            seen.push_back(stage);
          })
          .build_callbacks();

  REQUIRE(built.has_value());
  const auto event = wh::callbacks::make_event_view(seen);
  const auto info = make_info();
  built->on_start(wh::callbacks::stage::start, event, info);
  built->on_end(wh::callbacks::stage::end, event, info);
  built->on_error(wh::callbacks::stage::error, event, info);
  built->on_stream_start(wh::callbacks::stage::stream_start, event, info);
  built->on_stream_end(wh::callbacks::stage::stream_end, event, info);
  REQUIRE(seen == std::vector<wh::callbacks::stage>{
                      wh::callbacks::stage::start,
                      wh::callbacks::stage::end,
                      wh::callbacks::stage::error,
                      wh::callbacks::stage::stream_start,
                      wh::callbacks::stage::stream_end,
                  });

  builder.reset();
  REQUIRE(builder.empty());
  REQUIRE(builder.build_callbacks().has_error());
}
