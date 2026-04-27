#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/detail/plan_execute_graph.hpp"

namespace {

[[nodiscard]] auto run_pre(wh::compose::graph_add_node_options &options,
                           wh::compose::graph_process_state &process_state,
                           wh::compose::graph_value &payload) -> wh::core::result<void> {
  REQUIRE(static_cast<bool>(options.state.pre().handler));
  wh::compose::graph_state_cause cause{};
  wh::core::run_context context{};
  return options.state.pre().handler(cause, process_state, payload, context);
}

[[nodiscard]] auto run_post(wh::compose::graph_add_node_options &options,
                            wh::compose::graph_process_state &process_state,
                            wh::compose::graph_value &payload) -> wh::core::result<void> {
  REQUIRE(static_cast<bool>(options.state.post().handler));
  wh::compose::graph_state_cause cause{};
  wh::core::run_context context{};
  return options.state.post().handler(cause, process_state, payload, context);
}

} // namespace

TEST_CASE(
    "plan execute detail helpers read typed payloads and expose runtime context",
    "[UT][wh/adk/detail/"
    "plan_execute_graph.hpp][plan_execute_detail::make_context][condition][branch][boundary]") {
  using runtime_state = wh::adk::detail::plan_execute_detail::runtime_state;

  runtime_state state{
      .input_messages = {wh::testing::helper::make_text_message(wh::schema::message_role::user,
                                                                "input")},
      .current_plan = wh::agent::plan_execute_plan{.steps = {"a", "b"}},
      .executed_steps = {wh::agent::plan_execute_executed_step{
          .step = "a",
          .result = "done",
      }},
      .remaining_iterations = 3U,
  };

  auto context = wh::adk::detail::plan_execute_detail::make_context(state);
  REQUIRE(context.input_messages.size() == 1U);
  REQUIRE(context.current_plan.has_value());
  REQUIRE(context.current_plan->steps.size() == 2U);
  REQUIRE(context.executed_steps.size() == 1U);

  wh::compose::graph_value messages_payload = state.input_messages;
  auto messages = wh::adk::detail::plan_execute_detail::read_messages_payload(messages_payload);
  REQUIRE(messages.has_value());
  REQUIRE(messages->size() == 1U);

  wh::compose::graph_value invalid_payload = 42;
  auto invalid_messages =
      wh::adk::detail::plan_execute_detail::read_messages_payload(invalid_payload);
  REQUIRE(invalid_messages.has_error());
  REQUIRE(invalid_messages.error() == wh::core::errc::type_mismatch);

  wh::agent::agent_output output{};
  output.final_message =
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "answer");
  const wh::compose::graph_value output_payload = output;
  auto output_ref = wh::adk::detail::plan_execute_detail::read_agent_output(output_payload);
  REQUIRE(output_ref.has_value());
  REQUIRE(output_ref->get().final_message.role == wh::schema::message_role::assistant);

  const wh::compose::graph_value bad_output_payload = std::monostate{};
  auto bad_output = wh::adk::detail::plan_execute_detail::read_agent_output(bad_output_payload);
  REQUIRE(bad_output.has_error());
  REQUIRE(bad_output.error() == wh::core::errc::type_mismatch);

  wh::compose::graph_process_state parent{};
  REQUIRE(parent.emplace_workflow_state<runtime_state>(runtime_state{.remaining_iterations = 9U})
              .has_value());
  wh::compose::graph_process_state child{&parent};
  auto shared = wh::adk::detail::plan_execute_detail::read_state(child);
  REQUIRE(shared.has_value());
  REQUIRE(shared->get().remaining_iterations == 9U);
}

TEST_CASE("plan execute state callbacks bootstrap requests parse plans and emit final output",
          "[UT][wh/adk/detail/"
          "plan_execute_graph.hpp][plan_execute_detail::make_bootstrap_options][condition][branch]["
          "boundary]") {
  using runtime_state = wh::adk::detail::plan_execute_detail::runtime_state;
  using replanner_result = wh::adk::detail::plan_execute_detail::replanner_result;

  wh::compose::graph_process_state process_state{};
  wh::compose::graph_value payload = std::vector<wh::schema::message>{
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "seed"),
  };
  auto bootstrap = wh::adk::detail::plan_execute_detail::make_bootstrap_options(2U);
  REQUIRE(run_pre(bootstrap, process_state, payload).has_value());
  auto state = process_state.workflow_state_ref<runtime_state>();
  REQUIRE(state.has_value());
  REQUIRE(state->get().input_messages.size() == 1U);
  REQUIRE(state->get().remaining_iterations == 2U);
  REQUIRE(wh::core::any_cast<std::monostate>(&payload) != nullptr);

  auto request_options = wh::adk::detail::plan_execute_detail::make_request_options(
      wh::testing::helper::make_plan_request_builder(), true);
  REQUIRE(run_pre(request_options, process_state, payload).has_value());
  auto *request_messages = wh::core::any_cast<std::vector<wh::schema::message>>(&payload);
  REQUIRE(request_messages != nullptr);
  REQUIRE(request_messages->size() == 1U);
  REQUIRE(state->get().remaining_iterations == 1U);

  state->get().remaining_iterations = 0U;
  auto exhausted = run_pre(request_options, process_state, payload);
  REQUIRE(exhausted.has_error());
  REQUIRE(exhausted.error() == wh::core::errc::resource_exhausted);

  wh::agent::agent_output planner_output{};
  planner_output.final_message =
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "plan");
  payload = planner_output;
  auto parse_plan = wh::adk::detail::plan_execute_detail::make_parse_plan_options(
      wh::testing::helper::make_plan_reader());
  REQUIRE(run_post(parse_plan, process_state, payload).has_value());
  REQUIRE(state->get().current_plan.has_value());
  REQUIRE(state->get().current_plan->steps.size() == 2U);

  payload = planner_output;
  auto empty_plan = wh::adk::detail::plan_execute_detail::make_parse_plan_options(
      [](const wh::agent::agent_output &,
         wh::core::run_context &) -> wh::core::result<wh::agent::plan_execute_plan> {
        return wh::agent::plan_execute_plan{};
      });
  auto empty_status = run_post(empty_plan, process_state, payload);
  REQUIRE(empty_status.has_error());
  REQUIRE(empty_status.error() == wh::core::errc::contract_violation);

  state->get().current_plan = wh::agent::plan_execute_plan{.steps = {"step-a"}};
  payload = planner_output;
  auto capture = wh::adk::detail::plan_execute_detail::make_capture_step_options(
      wh::testing::helper::make_step_reader());
  REQUIRE(run_post(capture, process_state, payload).has_value());
  REQUIRE(state->get().executed_steps.size() == 1U);
  REQUIRE(state->get().executed_steps.front().step == "step-a");
  REQUIRE(state->get().current_plan->steps.empty());

  state->get().current_plan.reset();
  payload = planner_output;
  auto missing_plan = run_post(capture, process_state, payload);
  REQUIRE(missing_plan.has_error());
  REQUIRE(missing_plan.error() == wh::core::errc::contract_violation);

  state->get().current_plan = wh::agent::plan_execute_plan{.steps = {"next"}};
  payload = planner_output;
  auto replan = wh::adk::detail::plan_execute_detail::make_parse_replanner_options(
      [](const wh::agent::agent_output &,
         wh::core::run_context &) -> wh::core::result<wh::agent::plan_execute_decision> {
        return wh::agent::plan_execute_decision{
            .kind = wh::agent::plan_execute_decision_kind::plan,
            .next_plan = wh::agent::plan_execute_plan{.steps = {"redo"}},
        };
      },
      "reply");
  REQUIRE(run_post(replan, process_state, payload).has_value());
  auto *replanned = wh::core::any_cast<replanner_result>(&payload);
  REQUIRE(replanned != nullptr);
  REQUIRE(replanned->decision.kind == wh::agent::plan_execute_decision_kind::plan);
  REQUIRE(state->get().current_plan.has_value());
  REQUIRE(state->get().current_plan->steps == std::vector<std::string>{"redo"});

  payload = planner_output;
  auto respond = wh::adk::detail::plan_execute_detail::make_parse_replanner_options(
      wh::testing::helper::make_plan_execute_decision_reader(), "reply");
  REQUIRE(run_post(respond, process_state, payload).has_value());
  REQUIRE(state->get().final_output.has_value());
  auto reply_iter = state->get().final_output->output_values.find("reply");
  REQUIRE(reply_iter != state->get().final_output->output_values.end());
  REQUIRE(wh::core::any_cast<wh::schema::message>(&reply_iter->second) != nullptr);

  payload = planner_output;
  auto invalid_respond = wh::adk::detail::plan_execute_detail::make_parse_replanner_options(
      [](const wh::agent::agent_output &,
         wh::core::run_context &) -> wh::core::result<wh::agent::plan_execute_decision> {
        return wh::agent::plan_execute_decision{
            .kind = wh::agent::plan_execute_decision_kind::plan,
            .next_plan = wh::agent::plan_execute_plan{},
        };
      },
      "");
  auto invalid_plan_status = run_post(invalid_respond, process_state, payload);
  REQUIRE(invalid_plan_status.has_error());
  REQUIRE(invalid_plan_status.error() == wh::core::errc::contract_violation);

  wh::compose::graph_process_state empty_state{};
  payload = std::monostate{};
  auto emit_final = wh::adk::detail::plan_execute_detail::make_emit_final_options();
  auto missing_final = run_post(emit_final, empty_state, payload);
  REQUIRE(missing_final.has_error());

  auto emitted = run_post(emit_final, process_state, payload);
  REQUIRE(emitted.has_value());
  REQUIRE(wh::core::any_cast<wh::agent::agent_output>(&payload) != nullptr);
}

TEST_CASE("plan execute graph lowers authored shells and exposes executable binders",
          "[UT][wh/adk/detail/"
          "plan_execute_graph.hpp][bind_plan_execute_agent][condition][branch][boundary]") {
  wh::agent::plan_execute invalid{"invalid"};
  auto invalid_lower = wh::adk::detail::plan_execute_graph{invalid}.lower();
  REQUIRE(invalid_lower.has_error());

  auto configured = wh::testing::helper::make_configured_plan_execute("planner");
  REQUIRE(configured.has_value());
  REQUIRE(configured->freeze().has_value());
  auto lowered = wh::adk::detail::plan_execute_graph{configured.value()}.lower();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->compile().has_value());

  wh::agent::plan_execute mixed{"mixed"};
  auto planner = wh::testing::helper::make_executable_message_agent(
      "planner", wh::compose::node_contract::value, wh::compose::node_contract::stream,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "plan"));
  auto executor = wh::testing::helper::make_executable_message_agent(
      "executor", wh::compose::node_contract::stream, wh::compose::node_contract::value,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "step"));
  auto replanner = wh::testing::helper::make_executable_message_agent(
      "replanner", wh::compose::node_contract::stream, wh::compose::node_contract::stream,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "replan"));
  REQUIRE(planner.has_value());
  REQUIRE(executor.has_value());
  REQUIRE(replanner.has_value());
  REQUIRE(mixed.set_planner(std::move(planner).value()).has_value());
  REQUIRE(mixed.set_executor(std::move(executor).value()).has_value());
  REQUIRE(mixed.set_replanner(std::move(replanner).value()).has_value());
  REQUIRE(mixed.set_planner_request_builder(wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(mixed.set_executor_request_builder(wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(mixed.set_replanner_request_builder(wh::testing::helper::make_plan_request_builder())
              .has_value());
  REQUIRE(mixed.set_planner_plan_reader(wh::testing::helper::make_plan_reader()).has_value());
  REQUIRE(mixed.set_executor_step_reader(wh::testing::helper::make_step_reader()).has_value());
  REQUIRE(
      mixed.set_replanner_decision_reader(wh::testing::helper::make_plan_execute_decision_reader())
          .has_value());
  REQUIRE(mixed.freeze().has_value());
  auto mixed_lowered = wh::adk::detail::plan_execute_graph{mixed}.lower();
  REQUIRE(mixed_lowered.has_value());
  REQUIRE(mixed_lowered->compile().has_value());

  auto bound = wh::adk::detail::bind_plan_execute_agent(std::move(configured).value());
  REQUIRE(bound.has_value());
  auto bound_graph = bound->lower();
  REQUIRE(bound_graph.has_value());
  REQUIRE(bound_graph->compile().has_value());
}
