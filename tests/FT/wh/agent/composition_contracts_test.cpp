#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "wh/adk/agent_tool.hpp"
#include "wh/agent/agent.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/reflexion.hpp"
#include "wh/agent/plan_execute.hpp"
#include "wh/agent/reviewer_executor.hpp"
#include "wh/agent/self_refine.hpp"
#include "wh/agent/supervisor.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/core/any.hpp"
#include "wh/model/chat_model.hpp"

namespace {

[[nodiscard]] auto make_passthrough_graph(const std::string &name)
    -> wh::core::result<wh::compose::graph> {
  auto node = wh::compose::make_lambda_node(
      name,
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      });
  wh::compose::graph_boundary boundary{
      .input = node.input_contract(),
      .output = node.output_contract(),
  };
  wh::compose::graph graph{boundary, {}};
  auto added = graph.add_lambda(std::move(node));
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  auto start = graph.add_entry_edge(name);
  if (start.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(start.error());
  }
  auto finish = graph.add_exit_edge(name);
  if (finish.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(finish.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] auto make_executable_agent(const std::string &name)
    -> wh::core::result<wh::agent::agent> {
  wh::agent::agent authored{name};
  auto bound = authored.bind_execution(
      nullptr,
      [name]() mutable -> wh::core::result<wh::compose::graph> {
        return make_passthrough_graph(name + "_node");
      });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  return authored;
}

[[nodiscard]] auto require_executable_agent(const std::string &name)
    -> wh::agent::agent {
  auto authored = make_executable_agent(name);
  REQUIRE(authored.has_value());
  return std::move(authored).value();
}

[[nodiscard]] auto make_revision_request_builder()
    -> wh::agent::revision_request_builder {
  return [](const wh::agent::revision_context &, wh::core::run_context &)
      -> wh::core::result<std::vector<wh::schema::message>> {
    return std::vector<wh::schema::message>{};
  };
}

[[nodiscard]] auto make_review_decision_reader(
    const wh::agent::review_decision_kind kind)
    -> wh::agent::review_decision_reader {
  return [kind](const wh::agent::agent_output &, wh::core::run_context &)
      -> wh::core::result<wh::agent::review_decision> {
    return wh::agent::review_decision{.kind = kind};
  };
}

[[nodiscard]] auto make_plan_request_builder()
    -> wh::agent::plan_execute_request_builder {
  return [](const wh::agent::plan_execute_context &, wh::core::run_context &)
      -> wh::core::result<std::vector<wh::schema::message>> {
    return std::vector<wh::schema::message>{};
  };
}

[[nodiscard]] auto make_plan_reader() -> wh::agent::plan_execute_plan_reader {
  return [](const wh::agent::agent_output &, wh::core::run_context &)
      -> wh::core::result<wh::agent::plan_execute_plan> {
    return wh::agent::plan_execute_plan{.steps = {"step"}};
  };
}

[[nodiscard]] auto make_step_reader() -> wh::agent::plan_execute_step_reader {
  return [](const wh::agent::agent_output &, wh::core::run_context &)
      -> wh::core::result<std::string> { return std::string{"done"}; };
}

[[nodiscard]] auto make_plan_execute_decision_reader()
    -> wh::agent::plan_execute_decision_reader {
  return [](const wh::agent::agent_output &, wh::core::run_context &)
      -> wh::core::result<wh::agent::plan_execute_decision> {
    return wh::agent::plan_execute_decision{
        .kind = wh::agent::plan_execute_decision_kind::respond,
        .response = wh::schema::message{},
    };
  };
}

} // namespace

TEST_CASE("agent authoring freezes topology and transfer rules",
          "[core][agent][condition]") {
  wh::agent::agent root{"root"};
  REQUIRE(root.append_instruction("system").has_value());
  REQUIRE(root.add_child(wh::agent::agent{"planner"}).has_value());
  REQUIRE(root.allow_transfer_to_child("planner").has_value());
  REQUIRE(root.freeze().has_value());

  REQUIRE(root.frozen());
  REQUIRE(root.child_count() == 1U);
  auto planner = root.child("planner");
  REQUIRE(planner.has_value());
  REQUIRE(planner.value().get().frozen());
  REQUIRE(root.render_instruction() == "system");

  REQUIRE(root.add_child(wh::agent::agent{"late"}).has_error());
  REQUIRE(root.add_child(wh::agent::agent{"late"}).error() ==
          wh::core::errc::contract_violation);
  REQUIRE(root.allow_transfer_to_child("late").has_error());
  REQUIRE(root.allow_transfer_to_child("late").error() ==
          wh::core::errc::contract_violation);
}

TEST_CASE("agent authoring rejects duplicate children and missing transfer targets",
          "[core][agent][boundary]") {
  wh::agent::agent root{"root"};
  REQUIRE(root.add_child(wh::agent::agent{"planner"}).has_value());
  REQUIRE(root.add_child(wh::agent::agent{"planner"}).has_error());
  REQUIRE(root.add_child(wh::agent::agent{"planner"}).error() ==
          wh::core::errc::already_exists);

  REQUIRE(root.allow_transfer_to_child("ghost").has_value());
  REQUIRE(root.freeze().has_error());
  REQUIRE(root.freeze().error() == wh::core::errc::not_found);
}

TEST_CASE("agent authoring rejects empty child names",
          "[core][agent][boundary]") {
  wh::agent::agent root{"root"};
  REQUIRE(root.add_child(wh::agent::agent{""}).has_error());
  REQUIRE(root.add_child(wh::agent::agent{""}).error() ==
          wh::core::errc::invalid_argument);
}

TEST_CASE("agent tool build hook freezes bound agent and emits stable schema",
          "[core][agent][condition]") {
  wh::adk::agent_tool request_tool{"delegate", "delegate request",
                                   wh::agent::agent{"worker"}};
  REQUIRE(request_tool.freeze().has_value());
  auto request_schema = request_tool.tool_schema();
  REQUIRE(request_schema.name == "delegate");
  REQUIRE(request_schema.parameters.size() == 1U);
  REQUIRE(request_schema.parameters.front().name == "request");

  wh::adk::agent_tool history_tool{"delegate_history", "delegate history",
                                   wh::agent::agent{"worker"}};
  REQUIRE(history_tool
              .set_input_mode(wh::adk::agent_tool_input_mode::message_history)
              .has_value());
  REQUIRE(history_tool.freeze().has_value());
  auto history_schema = history_tool.tool_schema();
  REQUIRE(history_schema.raw_parameters_json_schema.find("messages") !=
          std::string::npos);

  wh::schema::tool_schema_definition custom_schema{};
  custom_schema.parameters.push_back(wh::schema::tool_parameter_schema{
      .name = "count",
      .type = wh::schema::tool_parameter_type::integer,
      .description = "work count",
      .required = true,
  });
  wh::adk::agent_tool custom_tool{"delegate_custom", "delegate custom",
                                  wh::agent::agent{"worker"}};
  REQUIRE(custom_tool
              .set_input_mode(wh::adk::agent_tool_input_mode::custom_schema)
              .has_value());
  REQUIRE(custom_tool.set_custom_schema(std::move(custom_schema)).has_value());
  REQUIRE(custom_tool.freeze().has_value());
  auto resolved = custom_tool.tool_schema();
  REQUIRE(resolved.parameters.size() == 1U);
  REQUIRE(resolved.parameters.front().name == "count");
}

TEST_CASE("agent tool build hook rejects missing custom schema",
          "[core][agent][boundary]") {
  wh::adk::agent_tool custom_tool{"delegate_custom", "delegate custom",
                                  wh::agent::agent{"worker"}};
  REQUIRE(custom_tool
              .set_input_mode(wh::adk::agent_tool_input_mode::custom_schema)
              .has_value());
  REQUIRE(custom_tool.freeze().has_error());
  REQUIRE(custom_tool.freeze().error() == wh::core::errc::invalid_argument);
}

TEST_CASE("plan execute build hook freezes planner executor and effective replanner",
          "[core][agent][condition]") {
  wh::agent::plan_execute authored{"plan-execute"};
  REQUIRE(authored.set_planner(require_executable_agent("planner")).has_value());
  REQUIRE(authored.set_executor(require_executable_agent("executor")).has_value());
  REQUIRE(
      authored.set_planner_request_builder(make_plan_request_builder()).has_value());
  REQUIRE(
      authored.set_executor_request_builder(make_plan_request_builder()).has_value());
  REQUIRE(authored
              .set_replanner_request_builder(make_plan_request_builder())
              .has_value());
  REQUIRE(authored.set_planner_plan_reader(make_plan_reader()).has_value());
  REQUIRE(authored.set_executor_step_reader(make_step_reader()).has_value());
  REQUIRE(authored
              .set_replanner_decision_reader(make_plan_execute_decision_reader())
              .has_value());
  REQUIRE(authored.freeze().has_value());
  REQUIRE(authored.frozen());
  REQUIRE(authored.effective_replanner_name().has_value());
  REQUIRE(authored.effective_replanner_name().value() == "planner");
}

TEST_CASE("plan execute build hook rejects missing or conflicting roles",
          "[core][agent][boundary]") {
  wh::agent::plan_execute missing{"plan-execute"};
  REQUIRE(missing.set_planner(wh::agent::agent{"planner"}).has_value());
  REQUIRE(missing.freeze().has_error());
  REQUIRE(missing.freeze().error() == wh::core::errc::invalid_argument);

  wh::agent::plan_execute conflicting{"plan-execute"};
  REQUIRE(conflicting.set_planner(wh::agent::agent{"same"}).has_value());
  REQUIRE(conflicting.set_executor(wh::agent::agent{"same"}).has_value());
  REQUIRE(conflicting.set_planner_request_builder(make_plan_request_builder())
              .has_value());
  REQUIRE(conflicting.set_executor_request_builder(make_plan_request_builder())
              .has_value());
  REQUIRE(conflicting.set_replanner_request_builder(make_plan_request_builder())
              .has_value());
  REQUIRE(conflicting.set_planner_plan_reader(make_plan_reader()).has_value());
  REQUIRE(conflicting.set_executor_step_reader(make_step_reader()).has_value());
  REQUIRE(conflicting
              .set_replanner_decision_reader(
                  make_plan_execute_decision_reader())
              .has_value());
  REQUIRE(conflicting.freeze().has_error());
  REQUIRE(conflicting.freeze().error() == wh::core::errc::contract_violation);

  wh::agent::plan_execute non_executable{"plan-execute"};
  REQUIRE(non_executable.set_planner(wh::agent::agent{"planner"}).has_value());
  REQUIRE(
      non_executable.set_executor(wh::agent::agent{"executor"}).has_value());
  REQUIRE(non_executable
              .set_planner_request_builder(make_plan_request_builder())
              .has_value());
  REQUIRE(non_executable
              .set_executor_request_builder(make_plan_request_builder())
              .has_value());
  REQUIRE(non_executable
              .set_replanner_request_builder(make_plan_request_builder())
              .has_value());
  REQUIRE(non_executable.set_planner_plan_reader(make_plan_reader()).has_value());
  REQUIRE(non_executable.set_executor_step_reader(make_step_reader()).has_value());
  REQUIRE(non_executable
              .set_replanner_decision_reader(
                  make_plan_execute_decision_reader())
              .has_value());
  REQUIRE(non_executable.freeze().has_error());
  REQUIRE(non_executable.freeze().error() ==
          wh::core::errc::contract_violation);
}

TEST_CASE("self refine build hook requires explicit revision contracts and lowers into agent",
          "[core][agent][self_refine]") {
  wh::agent::self_refine missing{"self-refine"};
  REQUIRE(missing.set_worker(require_executable_agent("worker")).has_value());
  REQUIRE(missing.freeze().has_error());
  REQUIRE(missing.freeze().error() == wh::core::errc::contract_violation);

  wh::agent::self_refine authored{"self-refine"};
  REQUIRE(authored.set_worker(require_executable_agent("worker")).has_value());
  REQUIRE(
      authored.set_worker_request_builder(make_revision_request_builder()).has_value());
  REQUIRE(authored
              .set_reviewer_request_builder(make_revision_request_builder())
              .has_value());
  REQUIRE(authored
              .set_review_decision_reader(
                  make_review_decision_reader(
                      wh::agent::review_decision_kind::accept))
              .has_value());
  REQUIRE(authored.freeze().has_value());
  REQUIRE(authored.effective_reviewer().has_value());
  REQUIRE(authored.effective_reviewer().value().get().name() == "worker");

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().executable());
  REQUIRE(lowered.value().lower_graph().has_value());
}

TEST_CASE("reviewer executor build hook freezes explicit contracts and lowers into agent",
          "[core][agent][reviewer_executor]") {
  wh::agent::reviewer_executor authored{"reviewer-executor"};
  REQUIRE(authored.set_executor(require_executable_agent("executor")).has_value());
  REQUIRE(authored.set_reviewer(require_executable_agent("reviewer")).has_value());
  REQUIRE(authored
              .set_executor_request_builder(make_revision_request_builder())
              .has_value());
  REQUIRE(authored
              .set_reviewer_request_builder(make_revision_request_builder())
              .has_value());
  REQUIRE(authored
              .set_review_decision_reader(
                  make_review_decision_reader(
                      wh::agent::review_decision_kind::accept))
              .has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().lower_graph().has_value());
}

TEST_CASE("reflexion build hook keeps optional memory writer explicit",
          "[core][agent][reflexion]") {
  wh::agent::reflexion missing_memory_builder{"reflexion"};
  REQUIRE(missing_memory_builder.set_actor(require_executable_agent("actor"))
              .has_value());
  REQUIRE(missing_memory_builder.set_critic(require_executable_agent("critic"))
              .has_value());
  REQUIRE(missing_memory_builder
              .set_memory_writer(require_executable_agent("memory"))
              .has_value());
  REQUIRE(missing_memory_builder
              .set_actor_request_builder(make_revision_request_builder())
              .has_value());
  REQUIRE(missing_memory_builder
              .set_critic_request_builder(make_revision_request_builder())
              .has_value());
  REQUIRE(missing_memory_builder
              .set_review_decision_reader(
                  make_review_decision_reader(
                      wh::agent::review_decision_kind::revise))
              .has_value());
  REQUIRE(missing_memory_builder.freeze().has_error());
  REQUIRE(missing_memory_builder.freeze().error() ==
          wh::core::errc::contract_violation);

  wh::agent::reflexion authored{"reflexion"};
  REQUIRE(authored.set_actor(require_executable_agent("actor")).has_value());
  REQUIRE(authored.set_critic(require_executable_agent("critic")).has_value());
  REQUIRE(authored.set_memory_writer(require_executable_agent("memory"))
              .has_value());
  REQUIRE(authored
              .set_actor_request_builder(make_revision_request_builder())
              .has_value());
  REQUIRE(authored
              .set_critic_request_builder(make_revision_request_builder())
              .has_value());
  REQUIRE(authored
              .set_memory_writer_request_builder(
                  make_revision_request_builder())
              .has_value());
  REQUIRE(authored
              .set_review_decision_reader(
                  make_review_decision_reader(
                      wh::agent::review_decision_kind::accept))
              .has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().lower_graph().has_value());
}

TEST_CASE("supervisor build hook auto-wires upward return and worker delegation",
          "[core][agent][condition]") {
  wh::agent::supervisor authored{"supervisor"};
  REQUIRE(authored.set_supervisor(require_executable_agent("supervisor"))
              .has_value());
  REQUIRE(authored.add_worker(require_executable_agent("planner")).has_value());
  REQUIRE(authored.add_worker(require_executable_agent("executor")).has_value());
  REQUIRE(authored.freeze().has_value());
  REQUIRE(authored.frozen());
  REQUIRE(authored.supervisor_agent().has_value());
  REQUIRE_FALSE(authored.supervisor_agent().value().get().allows_transfer_to_child(
      "planner"));

  auto lowered = wh::agent::make_agent(std::move(authored));
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().allows_transfer_to_child("planner"));
  REQUIRE(lowered.value().allows_transfer_to_child("executor"));
  auto planner = lowered.value().child("planner");
  REQUIRE(planner.has_value());
  REQUIRE(planner.value().get().allows_transfer_to_parent());
}

TEST_CASE("supervisor build hook requires at least one worker",
          "[core][agent][boundary]") {
  wh::agent::supervisor authored{"supervisor"};
  REQUIRE(authored.freeze().has_error());
  REQUIRE(authored.freeze().error() == wh::core::errc::invalid_argument);
}
