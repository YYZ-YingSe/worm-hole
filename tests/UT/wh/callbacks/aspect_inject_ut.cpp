#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "wh/callbacks/aspect_inject.hpp"
#include "wh/callbacks/manager.hpp"

namespace {

auto make_recording_context(std::vector<wh::callbacks::stage> &seen)
    -> wh::core::run_context {
  wh::core::run_context context{};
  context.callbacks.emplace();

  wh::callbacks::stage_callbacks callbacks{};
  const auto record_stage =
      [&seen](const wh::callbacks::stage stage, const wh::callbacks::event_view,
              const wh::callbacks::run_info &) { seen.push_back(stage); };
  callbacks.on_start = record_stage;
  callbacks.on_end = record_stage;
  callbacks.on_error = record_stage;
  callbacks.on_stream_start = record_stage;
  callbacks.on_stream_end = record_stage;
  context.callbacks->manager.register_local_callbacks(
      wh::callbacks::make_callback_config(
          [](const wh::callbacks::stage) noexcept { return true; }),
      std::move(callbacks));
  return context;
}

auto make_run_info() -> wh::callbacks::run_info {
  wh::callbacks::run_info info{};
  info.name = "inject";
  info.type = "inject";
  info.component = wh::core::component_kind::custom;
  return info;
}

} // namespace

TEST_CASE("aspect inject emit tolerates run contexts without callback state",
          "[UT][wh/callbacks/aspect_inject.hpp][emit][condition][branch][boundary]") {
  wh::core::run_context context{};
  const auto info = make_run_info();

  wh::callbacks::emit(context, wh::callbacks::stage::start, 1, info);
  wh::callbacks::emit_start(context, 2, info);
  wh::callbacks::emit_end(context, 3, info);
  wh::callbacks::emit_error(context, 4, info);
  wh::callbacks::emit_stream_start(context, 5, info);
  wh::callbacks::emit_stream_end(context, 6, info);

  SUCCEED();
}

TEST_CASE("aspect inject emit forwards the explicitly requested stage",
          "[UT][wh/callbacks/aspect_inject.hpp][emit][condition][branch][boundary]") {
  std::vector<wh::callbacks::stage> seen{};
  auto context = make_recording_context(seen);

  wh::callbacks::emit(context, wh::callbacks::stage::error, 7, make_run_info());

  REQUIRE(seen == std::vector<wh::callbacks::stage>{
                      wh::callbacks::stage::error,
                  });
}

TEST_CASE(
    "aspect inject stage helpers emit the full lifecycle in helper order",
    "[UT][wh/callbacks/aspect_inject.hpp][emit_start][condition][branch][boundary]") {
  std::vector<wh::callbacks::stage> seen{};
  auto context = make_recording_context(seen);
  const auto info = make_run_info();

  wh::callbacks::emit_start(context, 1, info);
  wh::callbacks::emit_end(context, 1, info);
  wh::callbacks::emit_error(context, 1, info);
  wh::callbacks::emit_stream_start(context, 1, info);
  wh::callbacks::emit_stream_end(context, 1, info);

  REQUIRE(seen == std::vector<wh::callbacks::stage>{
                      wh::callbacks::stage::start,
                      wh::callbacks::stage::end,
                      wh::callbacks::stage::error,
                      wh::callbacks::stage::stream_start,
                      wh::callbacks::stage::stream_end,
                  });
}
