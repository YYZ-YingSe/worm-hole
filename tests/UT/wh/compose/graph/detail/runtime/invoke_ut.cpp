#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/invoke.hpp"

TEST_CASE("invoke runtime action helpers preserve payload and terminal metadata",
          "[UT][wh/compose/graph/detail/runtime/invoke.hpp][ready_action::launch][branch][boundary]") {
  using wh::compose::detail::invoke_runtime::node_frame;
  using wh::compose::detail::invoke_runtime::pregel_action;
  using wh::compose::detail::invoke_runtime::ready_action;
  using wh::compose::detail::invoke_runtime::ready_action_kind;
  using wh::compose::detail::invoke_runtime::stage;

  auto no_ready = ready_action::no_ready();
  REQUIRE(no_ready.kind == ready_action_kind::no_ready);
  REQUIRE_FALSE(no_ready.frame.has_value());

  auto continue_scan = ready_action::continue_scan();
  REQUIRE(continue_scan.kind == ready_action_kind::continue_scan);
  REQUIRE_FALSE(continue_scan.frame.has_value());

  auto terminal =
      ready_action::terminal_error(wh::core::errc::contract_violation);
  REQUIRE(terminal.kind == ready_action_kind::terminal_error);
  REQUIRE(terminal.error == wh::core::errc::contract_violation);

  node_frame launched_frame{};
  launched_frame.stage = stage::post_state;
  launched_frame.node_id = 7U;
  launched_frame.cause = wh::compose::graph_state_cause{
      .run_id = 11U,
      .step = 5U,
      .node_key = "worker",
  };
  launched_frame.node_input = wh::compose::graph_value{13};

  auto launched = ready_action::launch(std::move(launched_frame));
  REQUIRE(launched.kind == ready_action_kind::launch);
  REQUIRE(launched.frame.has_value());
  REQUIRE(launched.frame->stage == stage::post_state);
  REQUIRE(launched.frame->node_id == 7U);
  REQUIRE(launched.frame->cause.node_key == "worker");
  REQUIRE(*wh::core::any_cast<int>(&launched.frame->node_input.value()) == 13);

  auto waiting = pregel_action::waiting(3U);
  REQUIRE(waiting.action == pregel_action::kind::waiting);
  REQUIRE(waiting.node_id == 3U);
  REQUIRE_FALSE(waiting.frame.has_value());

  auto skipped = pregel_action::skip(
      9U, wh::compose::graph_state_cause{
              .run_id = 2U,
              .step = 4U,
              .node_key = "skip",
          });
  REQUIRE(skipped.action == pregel_action::kind::skip);
  REQUIRE(skipped.node_id == 9U);
  REQUIRE(skipped.cause.node_key == "skip");

  node_frame pregel_frame{};
  pregel_frame.stage = stage::node;
  pregel_frame.node_id = 7U;
  pregel_frame.cause = wh::compose::graph_state_cause{
      .run_id = 11U,
      .step = 5U,
      .node_key = "worker",
  };
  auto launched_pregel = pregel_action::launch(std::move(pregel_frame));
  REQUIRE(launched_pregel.action == pregel_action::kind::launch);
  REQUIRE(launched_pregel.node_id == 7U);
  REQUIRE(launched_pregel.cause.node_key == "worker");
  REQUIRE(launched_pregel.frame.has_value());

  auto failed = pregel_action::terminal_error(
      5U,
      wh::compose::graph_state_cause{
          .run_id = 4U,
          .step = 8U,
          .node_key = "fail",
      },
      wh::core::errc::timeout);
  REQUIRE(failed.action == pregel_action::kind::terminal_error);
  REQUIRE(failed.node_id == 5U);
  REQUIRE(failed.cause.step == 8U);
  REQUIRE(failed.error == wh::core::errc::timeout);
}

TEST_CASE("invoke runtime state carriers default initialize to safe empty state",
          "[UT][wh/compose/graph/detail/runtime/invoke.hpp][invoke_state][condition][branch][boundary]") {
  wh::compose::detail::runtime_state::invoke_state invoke{};
  REQUIRE(invoke.services == nullptr);
  REQUIRE(invoke.parent_state == nullptr);
  REQUIRE(invoke.forwarded_checkpoints == nullptr);
  REQUIRE_FALSE(invoke.graph_scheduler.has_value());
  REQUIRE_FALSE(invoke.retain_inputs);
  REQUIRE(invoke.step_count == 0U);
  REQUIRE(invoke.step_budget == 0U);
  REQUIRE_FALSE(invoke.owned_call_options);
  REQUIRE_FALSE(invoke.start_entry_selection.has_value());

  wh::compose::detail::invoke_runtime::state_step step{};
  REQUIRE_FALSE(step.sender.has_value());
  REQUIRE_FALSE(step.payload.has_value());
  REQUIRE(step.frame.node_id == 0U);
}

TEST_CASE("invoke runtime node frame and actions default optional carriers to empty",
          "[UT][wh/compose/graph/detail/runtime/invoke.hpp][node_frame][condition][branch][boundary]") {
  using wh::compose::detail::invoke_runtime::node_frame;
  using wh::compose::detail::invoke_runtime::pregel_action;
  using wh::compose::detail::invoke_runtime::ready_action;
  using wh::compose::detail::invoke_runtime::ready_action_kind;
  using wh::compose::detail::invoke_runtime::stage;

  node_frame frame{};
  REQUIRE(frame.stage == stage::input);
  REQUIRE(frame.node_id == 0U);
  REQUIRE(frame.node == nullptr);
  REQUIRE(frame.state_handlers == nullptr);
  REQUIRE(frame.retry_budget == 0U);
  REQUIRE(frame.attempt == 0U);
  REQUIRE_FALSE(frame.timeout_budget.has_value());
  REQUIRE_FALSE(frame.pre_state_reader.has_value());
  REQUIRE_FALSE(frame.input_lowering.has_value());
  REQUIRE_FALSE(frame.node_input.has_value());

  auto no_ready = ready_action::no_ready();
  REQUIRE(no_ready.kind == ready_action_kind::no_ready);
  REQUIRE_FALSE(no_ready.frame.has_value());
  REQUIRE(no_ready.error == wh::core::errc::ok);

  auto terminal =
      ready_action::terminal_error(wh::core::errc::invalid_argument);
  REQUIRE(terminal.kind == ready_action_kind::terminal_error);
  REQUIRE_FALSE(terminal.frame.has_value());
  REQUIRE(terminal.error == wh::core::errc::invalid_argument);

  auto waiting = pregel_action::waiting(6U);
  REQUIRE(waiting.action == pregel_action::kind::waiting);
  REQUIRE(waiting.node_id == 6U);
  REQUIRE_FALSE(waiting.frame.has_value());
  REQUIRE(waiting.error == wh::core::errc::ok);
}
