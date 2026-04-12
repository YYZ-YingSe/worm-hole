#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>

#include "wh/core/callback.hpp"

namespace {

struct callback_config_probe {
  wh::core::callback_timing_checker timing_checker{
      [](wh::core::callback_stage) noexcept { return true; }};
  std::string name{"probe"};
};

using wh::core::apply_callback_run_metadata;
using wh::core::callback_event_as;
using wh::core::callback_event_cref_as;
using wh::core::callback_event_get_if;
using wh::core::callback_fatal_error;
using wh::core::callback_run_info;
using wh::core::callback_run_metadata;
using wh::core::callback_stage;
using wh::core::component_kind;
using wh::core::component_options;
using wh::core::is_reverse_callback_stage;
using wh::core::make_callback_event_payload;
using wh::core::make_callback_event_view;

static_assert(wh::core::TimingChecker<decltype(
              [](callback_stage) noexcept { return true; })>);
static_assert(wh::core::StageViewCallbackLike<decltype(
              [](callback_stage, wh::core::callback_event_view,
                 const callback_run_info &) {})>);
static_assert(wh::core::StagePayloadCallbackLike<decltype(
              [](callback_stage, wh::core::callback_event_payload,
                 const callback_run_info &) {})>);
static_assert(wh::core::CallbackConfigLike<callback_config_probe>);

} // namespace

TEST_CASE("callback stage helpers expose lifecycle ordering",
          "[UT][wh/core/callback.hpp][callback_stage][branch]") {
  REQUIRE(is_reverse_callback_stage(callback_stage::start));
  REQUIRE_FALSE(is_reverse_callback_stage(callback_stage::end));
  REQUIRE_FALSE(is_reverse_callback_stage(callback_stage::error));
  REQUIRE_FALSE(is_reverse_callback_stage(callback_stage::stream_start));
  REQUIRE_FALSE(is_reverse_callback_stage(callback_stage::stream_end));
}

TEST_CASE("callback event view borrows mutable and const lvalues",
          "[UT][wh/core/callback.hpp][make_callback_event_view][branch]") {
  int value = 7;
  const int const_value = 9;

  auto mutable_view = make_callback_event_view(value);
  auto const_view = make_callback_event_view(const_value);

  REQUIRE(callback_event_get_if<int>(mutable_view) == &value);
  REQUIRE(callback_event_get_if<const int>(const_view) == &const_value);
}

TEST_CASE("callback event payload helpers expose typed access and errors",
          "[UT][wh/core/callback.hpp][callback_event_as][branch][boundary]") {
  auto payload = make_callback_event_payload(std::string{"payload"});
  const auto const_payload = make_callback_event_payload(11);

  auto string_view = callback_event_cref_as<std::string>(payload);
  REQUIRE(string_view.has_value());
  REQUIRE(string_view.value().get() == "payload");

  auto copied = callback_event_as<int>(const_payload);
  REQUIRE(copied.has_value());
  REQUIRE(copied.value() == 11);

  auto moved = callback_event_as<std::string>(std::move(payload));
  REQUIRE(moved.has_value());
  REQUIRE(moved.value() == "payload");

  auto mismatch = callback_event_as<double>(const_payload);
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("callback run info overlays metadata and component options",
          "[UT][wh/core/callback.hpp][apply_callback_run_metadata][branch]") {
  callback_run_info run_info{
      .name = "node",
      .type = "worker",
      .component = component_kind::tool,
      .trace_id = "trace-a",
      .span_id = "span-a",
      .parent_span_id = "parent-a",
      .node_path = wh::core::address{"root"},
  };

  const callback_run_metadata metadata{
      .trace_id = "trace-b",
      .span_id = "span-b",
      .parent_span_id = "parent-b",
      .node_path = wh::core::address{"root", "child"},
  };

  const auto with_metadata = apply_callback_run_metadata(run_info, metadata);
  REQUIRE(with_metadata.trace_id == "trace-b");
  REQUIRE(with_metadata.span_id == "span-b");
  REQUIRE(with_metadata.parent_span_id == "parent-b");
  REQUIRE(with_metadata.node_path == wh::core::address{"root", "child"});

  component_options options{};
  options.set_base({.callbacks_enabled = true, .trace_id = "trace-c",
                    .span_id = "span-c"});

  const auto resolved = wh::core::apply_component_run_info(run_info, options);
  REQUIRE(resolved.trace_id == "trace-c");
  REQUIRE(resolved.span_id == "span-c");
  REQUIRE(resolved.name == "node");
}

TEST_CASE("callback fatal error formatting includes mapped fields",
          "[UT][wh/core/callback.hpp][callback_fatal_error::to_string][boundary]") {
  const callback_fatal_error error{
      .code = wh::core::make_error(wh::core::errc::timeout),
      .exception_message = "boom",
      .call_stack = "frame-a",
  };

  const auto formatted = error.to_string();
  REQUIRE(formatted.find("code=timeout") != std::string::npos);
  REQUIRE(formatted.find("exception=boom") != std::string::npos);
  REQUIRE(formatted.find("stack=frame-a") != std::string::npos);
}

TEST_CASE("callback facade reexports callback concepts and preserves base info on empty overlays",
          "[UT][wh/core/callback.hpp][CallbackConfigLike][condition]") {
  STATIC_REQUIRE(wh::core::TimingChecker<decltype(
                [](callback_stage) noexcept { return true; })>);
  STATIC_REQUIRE(wh::core::StageViewCallbackLike<decltype(
                [](callback_stage, wh::core::callback_event_view,
                   const callback_run_info &) {})>);
  STATIC_REQUIRE(wh::core::StagePayloadCallbackLike<decltype(
                [](callback_stage, wh::core::callback_event_payload,
                   const callback_run_info &) {})>);
  STATIC_REQUIRE(wh::core::CallbackConfigLike<callback_config_probe>);

  callback_run_info run_info{
      .name = "node",
      .type = "worker",
      .component = component_kind::tool,
      .trace_id = "trace-a",
      .span_id = "span-a",
  };
  component_options options{};
  options.set_base({.callbacks_enabled = false});

  const auto resolved = wh::core::apply_component_run_info(run_info, options);
  REQUIRE(resolved.trace_id == "trace-a");
  REQUIRE(resolved.span_id == "span-a");
  REQUIRE(resolved.name == "node");
}
