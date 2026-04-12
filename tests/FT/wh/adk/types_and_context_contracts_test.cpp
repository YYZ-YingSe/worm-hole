#include <catch2/catch_test_macros.hpp>

#include <utility>

#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/adk/event_stream.hpp"
#include "wh/adk/runner.hpp"
#include "wh/core/run_context.hpp"

namespace {

[[nodiscard]] auto make_ready_stream() -> wh::adk::agent_event_stream_reader {
  auto [writer, reader] = wh::adk::make_agent_event_stream();
  auto sent = wh::adk::send_agent_event(
      writer, wh::adk::make_control_event(
                  wh::adk::control_action{
                      .kind = wh::adk::control_action_kind::exit,
                  }));
  REQUIRE(sent.has_value());
  REQUIRE(wh::adk::close_agent_event_stream(writer).has_value());
  return std::move(reader);
}

[[nodiscard]] auto make_ready_output() -> wh::adk::agent_run_output {
  return wh::adk::agent_run_output{
      .events = make_ready_stream(),
  };
}

struct context_forward_runner_impl {
  mutable const wh::core::run_context *context_address{nullptr};

  [[nodiscard]] auto run(const wh::compose::graph_invoke_request &,
                         wh::core::run_context &context) const {
    context_address = std::addressof(context);
    return stdexec::just(wh::adk::agent_run_result{make_ready_output()});
  }
};

} // namespace

TEST_CASE("adk runner forwards the caller run context without cloning child state",
          "[core][adk][condition]") {
  context_forward_runner_impl impl{};
  wh::adk::runner<context_forward_runner_impl> runner{impl};

  wh::core::run_context parent{};

  wh::adk::run_request request{};

  auto started =
      wh::testing::helper::wait_value_on_test_thread(runner.run(request, parent));
  REQUIRE(started.has_value());
  REQUIRE(runner.implementation().context_address == std::addressof(parent));
}

TEST_CASE("adk custom event transport preserves payload and metadata",
          "[core][adk][condition]") {
  auto [writer, reader] = wh::adk::make_agent_event_stream();
  auto event = wh::adk::make_custom_event(
      "business.custom", wh::core::any{9},
      wh::adk::event_metadata{
          .run_path = wh::adk::run_path{{"root", "agent"}},
          .agent_name = "planner",
      });

  REQUIRE(wh::adk::send_agent_event(writer, std::move(event)).has_value());
  auto next = wh::adk::read_agent_event_stream(reader);
  REQUIRE(next.has_value());
  REQUIRE(next.value().value.has_value());

  const auto *custom =
      std::get_if<wh::adk::custom_event>(&next.value().value->payload);
  REQUIRE(custom != nullptr);
  REQUIRE(custom->name == "business.custom");
  const auto *typed = wh::core::any_cast<int>(&custom->payload);
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 9);
  REQUIRE(next.value().value->metadata.agent_name == "planner");
  REQUIRE(next.value().value->metadata.run_path ==
          wh::adk::run_path{{"root", "agent"}});
}
