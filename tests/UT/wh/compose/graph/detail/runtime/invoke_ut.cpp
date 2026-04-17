#include <catch2/catch_test_macros.hpp>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/runtime/invoke.hpp"
#include "wh/compose/graph/detail/runtime/invoke_session.hpp"

namespace {

class invoke_session_probe final
    : public wh::compose::detail::invoke_runtime::invoke_session {
public:
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_session;
  using wh::compose::detail::invoke_runtime::invoke_session::begin_state_pre;
  using wh::compose::detail::invoke_runtime::invoke_session::finalize_node_attempt;
  using wh::compose::detail::invoke_runtime::invoke_session::make_input_attempt;
  using wh::compose::detail::invoke_runtime::invoke_session::prepare_execution_input;
  using wh::compose::detail::invoke_runtime::invoke_session::slot;
  using wh::compose::detail::invoke_runtime::invoke_session::store_attempt_input;
};

} // namespace

TEST_CASE("invoke runtime action helpers preserve payload and terminal metadata",
          "[UT][wh/compose/graph/detail/runtime/invoke.hpp][ready_action::launch][branch][boundary]") {
  using wh::compose::detail::invoke_runtime::attempt_id;
  using wh::compose::detail::invoke_runtime::attempt_slot;
  using wh::compose::detail::invoke_runtime::pregel_action;
  using wh::compose::detail::invoke_runtime::ready_action;
  using wh::compose::detail::invoke_runtime::ready_action_kind;
  using wh::compose::detail::invoke_runtime::stage;

  auto no_ready = ready_action::no_ready();
  REQUIRE(no_ready.kind == ready_action_kind::no_ready);
  REQUIRE_FALSE(no_ready.attempt.has_value());

  auto continue_scan = ready_action::continue_scan();
  REQUIRE(continue_scan.kind == ready_action_kind::continue_scan);
  REQUIRE_FALSE(continue_scan.attempt.has_value());

  auto terminal =
      ready_action::terminal_error(wh::core::errc::contract_violation);
  REQUIRE(terminal.kind == ready_action_kind::terminal_error);
  REQUIRE(terminal.error == wh::core::errc::contract_violation);

  auto launched = ready_action::launch(attempt_id{7U});
  REQUIRE(launched.kind == ready_action_kind::launch);
  REQUIRE(launched.attempt == attempt_id{7U});

  auto waiting = pregel_action::waiting(3U);
  REQUIRE(waiting.action == pregel_action::kind::waiting);
  REQUIRE(waiting.node_id == 3U);
  REQUIRE_FALSE(waiting.attempt.has_value());

  auto skipped = pregel_action::skip(
      9U, wh::compose::graph_state_cause{
              .run_id = 2U,
              .step = 4U,
              .node_key = "skip",
          });
  REQUIRE(skipped.action == pregel_action::kind::skip);
  REQUIRE(skipped.node_id == 9U);
  REQUIRE(skipped.cause.node_key == "skip");

  auto launched_pregel = pregel_action::launch(
      7U,
      wh::compose::graph_state_cause{
      .run_id = 11U,
      .step = 5U,
      .node_key = "worker",
      },
      attempt_id{7U});
  REQUIRE(launched_pregel.action == pregel_action::kind::launch);
  REQUIRE(launched_pregel.node_id == 7U);
  REQUIRE(launched_pregel.cause.node_key == "worker");
  REQUIRE(launched_pregel.attempt == attempt_id{7U});

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
  REQUIRE_FALSE(invoke.control_scheduler.has_value());
  REQUIRE_FALSE(invoke.work_scheduler.has_value());
  REQUIRE_FALSE(invoke.retain_inputs);
  REQUIRE(invoke.step_count == 0U);
  REQUIRE(invoke.step_budget == 0U);
  REQUIRE_FALSE(invoke.owned_call_options);
  REQUIRE_FALSE(invoke.start_entry_selection.has_value());

  wh::compose::detail::invoke_runtime::state_step step{};
  REQUIRE_FALSE(step.sender.has_value());
  REQUIRE_FALSE(step.attempt.has_value());
}

TEST_CASE("invoke runtime attempt slot and actions default initialize to empty carriers",
          "[UT][wh/compose/graph/detail/runtime/invoke.hpp][attempt_slot][condition][branch][boundary]") {
  using wh::compose::detail::invoke_runtime::attempt_id;
  using wh::compose::detail::invoke_runtime::attempt_slot;
  using wh::compose::detail::invoke_runtime::pregel_action;
  using wh::compose::detail::invoke_runtime::ready_action;
  using wh::compose::detail::invoke_runtime::ready_action_kind;
  using wh::compose::detail::invoke_runtime::stage;

  attempt_id empty_attempt{};
  REQUIRE_FALSE(empty_attempt.has_value());

  attempt_slot slot{};
  REQUIRE(slot.stage == stage::input);
  REQUIRE(slot.node_id == 0U);
  REQUIRE(slot.node == nullptr);
  REQUIRE(slot.state_handlers == nullptr);
  REQUIRE(slot.retry_budget == 0U);
  REQUIRE(slot.attempt == 0U);
  REQUIRE_FALSE(slot.timeout_budget.has_value());
  REQUIRE_FALSE(slot.input.has_value());

  auto no_ready = ready_action::no_ready();
  REQUIRE(no_ready.kind == ready_action_kind::no_ready);
  REQUIRE_FALSE(no_ready.attempt.has_value());
  REQUIRE(no_ready.error == wh::core::errc::ok);

  auto terminal =
      ready_action::terminal_error(wh::core::errc::invalid_argument);
  REQUIRE(terminal.kind == ready_action_kind::terminal_error);
  REQUIRE_FALSE(terminal.attempt.has_value());
  REQUIRE(terminal.error == wh::core::errc::invalid_argument);

  auto waiting = pregel_action::waiting(6U);
  REQUIRE(waiting.action == pregel_action::kind::waiting);
  REQUIRE(waiting.node_id == 6U);
  REQUIRE_FALSE(waiting.attempt.has_value());
  REQUIRE(waiting.error == wh::core::errc::ok);
}

TEST_CASE("invoke session stage helpers keep live input inside attempt slot",
          "[UT][wh/compose/graph/detail/runtime/invoke.hpp][attempt_input][session_stage][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "invoke_slot_owned_input");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  invoke_session_probe session{
      &graph.value(),
      wh::compose::graph_value{3},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };

  const auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  auto attempt = session.make_input_attempt(worker_id.value(), 1U);
  REQUIRE(attempt.has_value());

  auto stored = session.store_attempt_input(attempt.value(),
                                            wh::compose::graph_value{7});
  REQUIRE(stored.has_value());
  auto &slot = session.slot(attempt.value());
  REQUIRE(slot.input.has_value());
  REQUIRE(slot.input->payload.has_value());
  auto before_pre =
      wh::testing::helper::read_graph_value<int>(*slot.input->payload);
  REQUIRE(before_pre.has_value());
  REQUIRE(before_pre.value() == 7);

  auto prepared = session.begin_state_pre(attempt.value());
  REQUIRE(prepared.has_value());
  REQUIRE(prepared->attempt == attempt.value());
  REQUIRE_FALSE(prepared->sender.has_value());
  REQUIRE(slot.input.has_value());
  REQUIRE(slot.input->payload.has_value());
  auto after_pre =
      wh::testing::helper::read_graph_value<int>(*slot.input->payload);
  REQUIRE(after_pre.has_value());
  REQUIRE(after_pre.value() == 7);

  auto normalized = session.prepare_execution_input(attempt.value());
  REQUIRE(normalized.has_value());
  REQUIRE(normalized->attempt == attempt.value());
  REQUIRE_FALSE(normalized->sender.has_value());
  REQUIRE(slot.input.has_value());
  REQUIRE(slot.input->payload.has_value());
  auto after_prepare =
      wh::testing::helper::read_graph_value<int>(*slot.input->payload);
  REQUIRE(after_prepare.has_value());
  REQUIRE(after_prepare.value() == 7);

  auto finalized = session.finalize_node_attempt(attempt.value());
  REQUIRE(finalized.has_value());
  REQUIRE(slot.stage ==
          wh::compose::detail::invoke_runtime::stage::node);
  REQUIRE(slot.input.has_value());
  REQUIRE(slot.input->payload.has_value());
  auto after_finalize =
      wh::testing::helper::read_graph_value<int>(*slot.input->payload);
  REQUIRE(after_finalize.has_value());
  REQUIRE(after_finalize.value() == 7);
}

TEST_CASE("invoke session prepare_execution_input consumes lowering from attempt slot",
          "[UT][wh/compose/graph/detail/runtime/invoke.hpp][attempt_input][prepare_execution_input][stream]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "invoke_input_lowering");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  invoke_session_probe session{
      &graph.value(),
      wh::compose::graph_value{0},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };

  const auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  auto attempt = session.make_input_attempt(worker_id.value(), 1U);
  REQUIRE(attempt.has_value());

  auto reader =
      wh::compose::make_single_value_stream_reader(std::string{"slot-reader"});
  REQUIRE(reader.has_value());
  auto stored = session.store_attempt_input(attempt.value(),
                                            wh::compose::graph_value{
                                                std::move(reader).value()});
  REQUIRE(stored.has_value());

  auto &slot = session.slot(attempt.value());
  REQUIRE(slot.input.has_value());
  slot.input->lowering.emplace();

  auto prepared = session.prepare_execution_input(attempt.value());
  REQUIRE(prepared.has_value());
  REQUIRE(prepared->attempt == attempt.value());
  REQUIRE(prepared->sender.has_value());
  REQUIRE(slot.stage ==
          wh::compose::detail::invoke_runtime::stage::prepare);
  REQUIRE(slot.input.has_value());
  REQUIRE_FALSE(slot.input->lowering.has_value());
  REQUIRE_FALSE(slot.input->payload.has_value());

  auto waited = stdexec::sync_wait(std::move(*prepared->sender));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  auto *chunks = wh::core::any_cast<std::vector<wh::compose::graph_value>>(
      &status.value());
  REQUIRE(chunks != nullptr);
  REQUIRE(chunks->size() == 1U);
  auto typed =
      wh::testing::helper::read_graph_value<std::string>(chunks->front());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == "slot-reader");
}
