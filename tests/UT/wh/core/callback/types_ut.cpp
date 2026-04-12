#include <catch2/catch_test_macros.hpp>

#include <string>

#include "wh/core/callback/types.hpp"

namespace {

using wh::core::apply_callback_run_metadata;
using wh::core::callback_event_as;
using wh::core::callback_event_cref_as;
using wh::core::callback_event_get_if;
using wh::core::callback_fatal_error;
using wh::core::callback_run_info;
using wh::core::callback_run_metadata;
using wh::core::callback_stage;
using wh::core::component_kind;
using wh::core::make_callback_event_payload;
using wh::core::make_callback_event_view;

} // namespace

TEST_CASE("callback stage reversal marks start only",
          "[UT][wh/core/callback/types.hpp][is_reverse_callback_stage][branch]") {
  REQUIRE(wh::core::is_reverse_callback_stage(callback_stage::start));
  REQUIRE_FALSE(wh::core::is_reverse_callback_stage(callback_stage::end));
  REQUIRE_FALSE(wh::core::is_reverse_callback_stage(callback_stage::error));
  REQUIRE_FALSE(
      wh::core::is_reverse_callback_stage(callback_stage::stream_start));
  REQUIRE_FALSE(
      wh::core::is_reverse_callback_stage(callback_stage::stream_end));
}

TEST_CASE("callback event helpers expose borrowed and owning typed access",
          "[UT][wh/core/callback/types.hpp][callback_event_as][branch][boundary]") {
  int value = 7;
  auto view = make_callback_event_view(value);
  auto payload = make_callback_event_payload(std::string{"payload"});

  REQUIRE(callback_event_get_if<int>(view) == &value);

  auto string_cref = callback_event_cref_as<std::string>(payload);
  REQUIRE(string_cref.has_value());
  REQUIRE(string_cref.value().get() == "payload");

  auto string_move = callback_event_as<std::string>(std::move(payload));
  REQUIRE(string_move.has_value());
  REQUIRE(string_move.value() == "payload");

  auto mismatch = callback_event_as<double>(make_callback_event_payload(11));
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("callback metadata overlays only populated fields",
          "[UT][wh/core/callback/types.hpp][apply_callback_run_metadata][branch]") {
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

  const auto updated = apply_callback_run_metadata(run_info, metadata);
  REQUIRE(updated.trace_id == "trace-b");
  REQUIRE(updated.span_id == "span-b");
  REQUIRE(updated.parent_span_id == "parent-b");
  REQUIRE(updated.node_path == wh::core::address{"root", "child"});
}

TEST_CASE("callback fatal error formats diagnostic string",
          "[UT][wh/core/callback/types.hpp][callback_fatal_error::to_string][boundary]") {
  const callback_fatal_error error{
      .code = wh::core::make_error(wh::core::errc::timeout),
      .exception_message = "boom",
      .call_stack = "frame-a",
  };

  const auto text = error.to_string();
  REQUIRE(text.find("code=timeout") != std::string::npos);
  REQUIRE(text.find("exception=boom") != std::string::npos);
  REQUIRE(text.find("stack=frame-a") != std::string::npos);
}

TEST_CASE("callback metadata leaves run info unchanged when metadata is empty",
          "[UT][wh/core/callback/types.hpp][apply_callback_run_metadata][condition]") {
  callback_run_info run_info{
      .name = "node",
      .type = "worker",
      .component = component_kind::tool,
      .trace_id = "trace-a",
      .span_id = "span-a",
      .parent_span_id = "parent-a",
      .node_path = wh::core::address{"root"},
  };
  const callback_run_metadata metadata{};

  REQUIRE(metadata.empty());
  const auto updated = apply_callback_run_metadata(run_info, metadata);
  REQUIRE(updated.trace_id == "trace-a");
  REQUIRE(updated.span_id == "span-a");
  REQUIRE(updated.parent_span_id == "parent-a");
  REQUIRE(updated.node_path == wh::core::address{"root"});
}
