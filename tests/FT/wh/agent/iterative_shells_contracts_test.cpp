#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"

namespace {

using wh::testing::helper::invoke_agent_graph;
using wh::testing::helper::make_executable_chat_agent;
using wh::testing::helper::make_text_message;
using wh::testing::helper::message_text;

} // namespace

TEST_CASE("plan execute shell public binding executes planner executor and replanner loop",
          "[core][agent][plan_execute][functional]") {
  auto planner = make_executable_chat_agent("planner");
  REQUIRE(planner.has_value());
  auto executor = make_executable_chat_agent("executor");
  REQUIRE(executor.has_value());
  auto replanner = make_executable_chat_agent("replanner");
  REQUIRE(replanner.has_value());

  auto planner_requests = std::make_shared<std::size_t>(0U);
  auto executor_requests = std::make_shared<std::size_t>(0U);
  auto replanner_requests = std::make_shared<std::size_t>(0U);
  auto replanner_decisions = std::make_shared<std::size_t>(0U);

  wh::agent::plan_execute authored{"plan-execute"};
  REQUIRE(authored.set_planner(std::move(*planner)).has_value());
  REQUIRE(authored.set_executor(std::move(*executor)).has_value());
  REQUIRE(authored.set_replanner(std::move(*replanner)).has_value());
  REQUIRE(authored
              .set_planner_request_builder(
                  [planner_requests](const wh::agent::plan_execute_context &context,
                                     wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *planner_requests += 1U;
                    if (context.current_plan.has_value() ||
                        !context.executed_steps.empty() ||
                        context.input_messages.size() != 1U) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user, "plan")};
                  })
              .has_value());
  REQUIRE(authored
              .set_executor_request_builder(
                  [executor_requests](const wh::agent::plan_execute_context &context,
                                      wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *executor_requests += 1U;
                    if (!context.current_plan.has_value() ||
                        context.current_plan->steps.size() != 1U ||
                        context.executed_steps.size() != 0U) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user,
                                          context.current_plan->steps.front())};
                  })
              .has_value());
  REQUIRE(authored
              .set_replanner_request_builder(
                  [replanner_requests](const wh::agent::plan_execute_context &context,
                                       wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *replanner_requests += 1U;
                    if (!context.current_plan.has_value() ||
                        !context.current_plan->steps.empty() ||
                        context.executed_steps.size() != 1U) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user,
                                          "replan")};
                  })
              .has_value());
  REQUIRE(authored
              .set_planner_plan_reader(
                  [](const wh::agent::agent_output &, wh::core::run_context &)
                      -> wh::core::result<wh::agent::plan_execute_plan> {
                    return wh::agent::plan_execute_plan{.steps = {"inspect"}};
                  })
              .has_value());
  REQUIRE(authored
              .set_executor_step_reader(
                  [](const wh::agent::agent_output &, wh::core::run_context &)
                      -> wh::core::result<std::string> {
                    return std::string{"step-result"};
                  })
              .has_value());
  REQUIRE(authored
              .set_replanner_decision_reader(
                  [replanner_decisions](const wh::agent::agent_output &,
                                        wh::core::run_context &)
                      -> wh::core::result<wh::agent::plan_execute_decision> {
                    *replanner_decisions += 1U;
                    return wh::agent::plan_execute_decision{
                        .kind = wh::agent::plan_execute_decision_kind::respond,
                        .response = make_text_message(
                            wh::schema::message_role::assistant, "done"),
                    };
                  })
              .has_value());

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->executable());
  auto graph = lowered->lower_graph();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(
      graph.value(),
      {make_text_message(wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(message_text(output->final_message) == "done");
  REQUIRE(*planner_requests == 1U);
  REQUIRE(*executor_requests == 1U);
  REQUIRE(*replanner_requests == 1U);
  REQUIRE(*replanner_decisions == 1U);
}

TEST_CASE("self refine shell public binding executes worker fallback reviewer path",
          "[core][agent][self_refine][functional]") {
  auto worker = make_executable_chat_agent("worker");
  REQUIRE(worker.has_value());

  auto worker_requests = std::make_shared<std::size_t>(0U);
  auto reviewer_requests = std::make_shared<std::size_t>(0U);
  auto decisions = std::make_shared<std::size_t>(0U);

  wh::agent::self_refine authored{"self-refine"};
  REQUIRE(authored.set_worker(std::move(*worker)).has_value());
  REQUIRE(authored
              .set_worker_request_builder(
                  [worker_requests](const wh::agent::revision_context &context,
                                    wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *worker_requests += 1U;
                    if (context.current_draft != nullptr ||
                        context.current_review != nullptr ||
                        context.input_messages.size() != 1U) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user, "draft")};
                  })
              .has_value());
  REQUIRE(authored
              .set_reviewer_request_builder(
                  [reviewer_requests](const wh::agent::revision_context &context,
                                      wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *reviewer_requests += 1U;
                    if (context.current_draft == nullptr ||
                        context.current_review != nullptr) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user,
                                          "review")};
                  })
              .has_value());
  REQUIRE(authored
              .set_review_decision_reader(
                  [decisions](const wh::agent::agent_output &, wh::core::run_context &)
                      -> wh::core::result<wh::agent::review_decision> {
                    *decisions += 1U;
                    return wh::agent::review_decision{
                        .kind = wh::agent::review_decision_kind::accept,
                    };
                  })
              .has_value());

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  auto graph = lowered->lower_graph();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(
      graph.value(),
      {make_text_message(wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(message_text(output->final_message) == "ok");
  REQUIRE(*worker_requests == 1U);
  REQUIRE(*reviewer_requests == 1U);
  REQUIRE(*decisions == 1U);
}

TEST_CASE("reviewer executor shell public binding executes accepted executor draft",
          "[core][agent][reviewer_executor][functional]") {
  auto reviewer = make_executable_chat_agent("reviewer");
  REQUIRE(reviewer.has_value());
  auto executor = make_executable_chat_agent("executor");
  REQUIRE(executor.has_value());

  auto executor_requests = std::make_shared<std::size_t>(0U);
  auto reviewer_requests = std::make_shared<std::size_t>(0U);
  auto decisions = std::make_shared<std::size_t>(0U);

  wh::agent::reviewer_executor authored{"reviewer-executor"};
  REQUIRE(authored.set_reviewer(std::move(*reviewer)).has_value());
  REQUIRE(authored.set_executor(std::move(*executor)).has_value());
  REQUIRE(authored
              .set_executor_request_builder(
                  [executor_requests](const wh::agent::revision_context &context,
                                      wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *executor_requests += 1U;
                    if (context.current_draft != nullptr ||
                        context.current_review != nullptr ||
                        context.input_messages.size() != 1U) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user, "draft")};
                  })
              .has_value());
  REQUIRE(authored
              .set_reviewer_request_builder(
                  [reviewer_requests](const wh::agent::revision_context &context,
                                      wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *reviewer_requests += 1U;
                    if (context.current_draft == nullptr ||
                        context.current_review != nullptr) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user,
                                          "review")};
                  })
              .has_value());
  REQUIRE(authored
              .set_review_decision_reader(
                  [decisions](const wh::agent::agent_output &, wh::core::run_context &)
                      -> wh::core::result<wh::agent::review_decision> {
                    *decisions += 1U;
                    return wh::agent::review_decision{
                        .kind = wh::agent::review_decision_kind::accept,
                    };
                  })
              .has_value());

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  auto graph = lowered->lower_graph();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(
      graph.value(),
      {make_text_message(wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(message_text(output->final_message) == "ok");
  REQUIRE(*executor_requests == 1U);
  REQUIRE(*reviewer_requests == 1U);
  REQUIRE(*decisions == 1U);
}

TEST_CASE("reflexion shell public binding executes optional memory writer path",
          "[core][agent][reflexion][functional]") {
  auto actor = make_executable_chat_agent("actor");
  REQUIRE(actor.has_value());
  auto critic = make_executable_chat_agent("critic");
  REQUIRE(critic.has_value());
  auto memory = make_executable_chat_agent("memory");
  REQUIRE(memory.has_value());

  auto actor_requests = std::make_shared<std::size_t>(0U);
  auto critic_requests = std::make_shared<std::size_t>(0U);
  auto memory_requests = std::make_shared<std::size_t>(0U);
  auto decisions = std::make_shared<std::size_t>(0U);

  wh::agent::reflexion authored{"reflexion"};
  REQUIRE(authored.set_actor(std::move(*actor)).has_value());
  REQUIRE(authored.set_critic(std::move(*critic)).has_value());
  REQUIRE(authored.set_memory_writer(std::move(*memory)).has_value());
  REQUIRE(authored
              .set_actor_request_builder(
                  [actor_requests](const wh::agent::revision_context &context,
                                   wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *actor_requests += 1U;
                    if (*actor_requests == 1U &&
                        (context.current_draft != nullptr ||
                         context.current_review != nullptr)) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    if (*actor_requests == 2U &&
                        (context.current_draft == nullptr ||
                         context.current_review == nullptr)) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    if (*actor_requests > 2U) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user, "draft")};
                  })
              .has_value());
  REQUIRE(authored
              .set_critic_request_builder(
                  [critic_requests](const wh::agent::revision_context &context,
                                    wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *critic_requests += 1U;
                    if (context.current_draft == nullptr) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user,
                                          "critique")};
                  })
              .has_value());
  REQUIRE(authored
              .set_memory_writer_request_builder(
                  [memory_requests](const wh::agent::revision_context &context,
                                    wh::core::run_context &)
                      -> wh::core::result<std::vector<wh::schema::message>> {
                    *memory_requests += 1U;
                    if (context.current_draft == nullptr ||
                        context.current_review == nullptr) {
                      return wh::core::result<
                          std::vector<wh::schema::message>>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return std::vector<wh::schema::message>{
                        make_text_message(wh::schema::message_role::user,
                                          "memory")};
                  })
              .has_value());
  REQUIRE(authored
              .set_review_decision_reader(
                  [decisions](const wh::agent::agent_output &, wh::core::run_context &)
                      -> wh::core::result<wh::agent::review_decision> {
                    *decisions += 1U;
                    return wh::agent::review_decision{
                        .kind = *decisions == 1U
                                    ? wh::agent::review_decision_kind::revise
                                    : wh::agent::review_decision_kind::accept,
                    };
                  })
              .has_value());

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  auto graph = lowered->lower_graph();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(
      graph.value(),
      {make_text_message(wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(message_text(output->final_message) == "ok");
  REQUIRE(*actor_requests == 2U);
  REQUIRE(*critic_requests == 2U);
  REQUIRE(*memory_requests == 1U);
  REQUIRE(*decisions == 2U);
}
