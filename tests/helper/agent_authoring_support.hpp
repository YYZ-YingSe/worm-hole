#pragma once

#include <tuple>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/agent/bind.hpp"
#include "wh/agent/chat.hpp"
#include "wh/agent/plan_execute.hpp"
#include "wh/agent/react.hpp"
#include "wh/agent/reflexion.hpp"
#include "wh/agent/research.hpp"
#include "wh/agent/revision.hpp"
#include "wh/agent/reviewer_executor.hpp"
#include "wh/agent/self_refine.hpp"
#include "wh/agent/supervisor.hpp"
#include "wh/agent/swarm.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/core/component.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"
#include "wh/tool/tool.hpp"

namespace wh::testing::helper {

[[nodiscard]] inline auto make_configured_chat(std::string name,
                                               std::string description)
    -> wh::agent::chat;

[[nodiscard]] inline auto make_text_message(
    const wh::schema::message_role role, std::string text)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = role;
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

[[nodiscard]] inline auto message_text(const wh::schema::message &message)
    -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part);
        typed != nullptr) {
      text.append(typed->text);
    }
  }
  return text;
}

[[nodiscard]] inline auto make_passthrough_graph(const std::string &name)
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

[[nodiscard]] inline auto make_executable_agent(const std::string &name)
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

[[nodiscard]] inline auto make_executable_chat_agent(
    std::string name, std::string description = "assistant")
    -> wh::core::result<wh::agent::agent> {
  auto authored = make_configured_chat(std::move(name), std::move(description));
  return wh::agent::make_agent(std::move(authored));
}

[[nodiscard]] inline auto make_fixed_output_graph(const std::string &node_name,
                                                  wh::agent::agent_output output)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph graph{};
  auto node = wh::compose::make_lambda_node(
      node_name,
      [output = std::move(output)](wh::compose::graph_value &,
                                   wh::core::run_context &,
                                   const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return wh::compose::graph_value{output};
      });
  auto added = graph.add_lambda(std::move(node));
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  auto start = graph.add_entry_edge(node_name);
  if (start.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(start.error());
  }
  auto finish = graph.add_exit_edge(node_name);
  if (finish.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(finish.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] inline auto make_fixed_output_agent(const std::string &name,
                                                  wh::agent::agent_output output)
    -> wh::core::result<wh::agent::agent> {
  auto graph = make_fixed_output_graph(name + "_node", std::move(output));
  if (graph.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(graph.error());
  }

  wh::agent::agent authored{name};
  auto bound = authored.bind_execution(
      nullptr,
      [graph = std::move(graph).value()]() mutable
          -> wh::core::result<wh::compose::graph> { return graph; });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  return authored;
}

[[nodiscard]] inline auto invoke_agent_graph(
    wh::compose::graph &graph, std::vector<wh::schema::message> messages)
    -> wh::core::result<wh::agent::agent_output> {
  wh::compose::graph_invoke_request request{};
  request.input = wh::core::any{std::move(messages)};

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  if (!waited.has_value()) {
    return wh::core::result<wh::agent::agent_output>::failure(
        wh::core::errc::canceled);
  }

  auto invoke_status = std::get<0>(std::move(waited).value());
  if (invoke_status.has_error()) {
    return wh::core::result<wh::agent::agent_output>::failure(
        invoke_status.error());
  }
  auto invoke_result = std::move(invoke_status).value();
  if (invoke_result.output_status.has_error()) {
    return wh::core::result<wh::agent::agent_output>::failure(
        invoke_result.output_status.error());
  }

  auto *typed =
      wh::core::any_cast<wh::agent::agent_output>(&invoke_result.output_status.value());
  if (typed == nullptr) {
    return wh::core::result<wh::agent::agent_output>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::move(*typed);
}

[[nodiscard]] inline auto make_revision_request_builder()
    -> wh::agent::revision_request_builder {
  return [](const wh::agent::revision_context &context, wh::core::run_context &)
      -> wh::core::result<std::vector<wh::schema::message>> {
    return std::vector<wh::schema::message>{context.input_messages.begin(),
                                            context.input_messages.end()};
  };
}

[[nodiscard]] inline auto
make_review_decision_reader(const wh::agent::review_decision_kind kind)
    -> wh::agent::review_decision_reader {
  return [kind](const wh::agent::agent_output &, wh::core::run_context &)
      -> wh::core::result<wh::agent::review_decision> {
    return wh::agent::review_decision{.kind = kind};
  };
}

[[nodiscard]] inline auto make_plan_request_builder()
    -> wh::agent::plan_execute_request_builder {
  return [](const wh::agent::plan_execute_context &context,
            wh::core::run_context &)
      -> wh::core::result<std::vector<wh::schema::message>> {
    return std::vector<wh::schema::message>{context.input_messages.begin(),
                                            context.input_messages.end()};
  };
}

[[nodiscard]] inline auto make_plan_reader()
    -> wh::agent::plan_execute_plan_reader {
  return [](const wh::agent::agent_output &, wh::core::run_context &)
      -> wh::core::result<wh::agent::plan_execute_plan> {
    return wh::agent::plan_execute_plan{.steps = {"step-1", "step-2"}};
  };
}

[[nodiscard]] inline auto make_step_reader()
    -> wh::agent::plan_execute_step_reader {
  return [](const wh::agent::agent_output &, wh::core::run_context &)
      -> wh::core::result<std::string> { return std::string{"step-result"}; };
}

[[nodiscard]] inline auto make_plan_execute_decision_reader()
    -> wh::agent::plan_execute_decision_reader {
  return [](const wh::agent::agent_output &, wh::core::run_context &)
      -> wh::core::result<wh::agent::plan_execute_decision> {
    return wh::agent::plan_execute_decision{
        .kind = wh::agent::plan_execute_decision_kind::respond,
        .response = make_text_message(wh::schema::message_role::assistant,
                                      "done"),
    };
  };
}

struct probe_model_state {
  std::size_t bind_calls{0U};
};

class sync_probe_model {
public:
  explicit sync_probe_model(
      std::shared_ptr<probe_model_state> state = std::make_shared<probe_model_state>(),
      std::vector<wh::schema::tool_schema_definition> tools = {})
      : state_(std::move(state)), bound_tools_(std::move(tools)) {}

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"ProbeModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &,
                            wh::core::run_context &) const
      -> wh::model::chat_invoke_result {
    return wh::model::chat_response{
        .message = make_text_message(wh::schema::message_role::assistant, "ok")};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &,
                            wh::core::run_context &) const
      -> wh::model::chat_message_stream_result {
    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_values_stream_reader(
            std::vector<wh::schema::message>{
                make_text_message(wh::schema::message_role::assistant, "ok")})};
  }

  [[nodiscard]] auto bind_tools(
      std::span<const wh::schema::tool_schema_definition> tools) const
      -> sync_probe_model {
    ++state_->bind_calls;
    return sync_probe_model{
        state_,
        std::vector<wh::schema::tool_schema_definition>{tools.begin(),
                                                        tools.end()}};
  }

  [[nodiscard]] auto options() const noexcept
      -> const wh::model::chat_model_options & {
    return options_;
  }

  [[nodiscard]] auto bound_tools() const noexcept
      -> const std::vector<wh::schema::tool_schema_definition> & {
    return bound_tools_;
  }

  [[nodiscard]] auto state() const noexcept
      -> const std::shared_ptr<probe_model_state> & {
    return state_;
  }

private:
  std::shared_ptr<probe_model_state> state_{};
  std::vector<wh::schema::tool_schema_definition> bound_tools_{};
  wh::model::chat_model_options options_{};
};

struct sync_tool {
  wh::schema::tool_schema_definition schema_value{
      .name = "sync",
      .description = "sync tool",
  };

  [[nodiscard]] auto schema() const
      -> const wh::schema::tool_schema_definition & {
    return schema_value;
  }

  [[nodiscard]] auto invoke(wh::tool::tool_request request,
                            wh::core::run_context &) const
      -> wh::tool::tool_invoke_result {
    return request.input_json;
  }
};

struct async_stream_tool {
  wh::schema::tool_schema_definition schema_value{
      .name = "stream",
      .description = "stream tool",
  };

  [[nodiscard]] auto schema() const
      -> const wh::schema::tool_schema_definition & {
    return schema_value;
  }

  [[nodiscard]] auto async_stream(wh::tool::tool_request request,
                                  wh::core::run_context &) const {
    auto reader =
        wh::schema::stream::make_single_value_stream_reader<std::string>(
            request.input_json);
    return stdexec::just(wh::tool::tool_output_stream_result{
        wh::tool::tool_output_stream_reader{std::move(reader)}});
  }
};

[[nodiscard]] inline auto make_configured_chat(std::string name,
                                               std::string description)
    -> wh::agent::chat {
  wh::agent::chat authored{std::move(name), std::move(description)};
  [[maybe_unused]] const auto model_status =
      authored.set_model(sync_probe_model{});
  return authored;
}

[[nodiscard]] inline auto make_configured_react(std::string name,
                                                std::string description)
    -> wh::agent::react {
  wh::agent::react authored{std::move(name), std::move(description)};
  [[maybe_unused]] const auto model_status =
      authored.set_model(sync_probe_model{});
  [[maybe_unused]] const auto options_status =
      authored.set_tools_node_options(wh::agent::tools_node_authoring_options{});
  return authored;
}

[[nodiscard]] inline auto make_configured_plan_execute(std::string name)
    -> wh::core::result<wh::agent::plan_execute> {
  wh::agent::plan_execute authored{std::move(name)};
  auto planner = make_executable_agent("planner");
  if (planner.has_error()) {
    return wh::core::result<wh::agent::plan_execute>::failure(planner.error());
  }
  auto executor = make_executable_agent("executor");
  if (executor.has_error()) {
    return wh::core::result<wh::agent::plan_execute>::failure(executor.error());
  }
  auto planner_set = authored.set_planner(std::move(planner).value());
  if (planner_set.has_error()) {
    return wh::core::result<wh::agent::plan_execute>::failure(planner_set.error());
  }
  auto executor_set = authored.set_executor(std::move(executor).value());
  if (executor_set.has_error()) {
    return wh::core::result<wh::agent::plan_execute>::failure(
        executor_set.error());
  }
  authored.set_planner_request_builder(make_plan_request_builder());
  authored.set_executor_request_builder(make_plan_request_builder());
  authored.set_replanner_request_builder(make_plan_request_builder());
  authored.set_planner_plan_reader(make_plan_reader());
  authored.set_executor_step_reader(make_step_reader());
  authored.set_replanner_decision_reader(make_plan_execute_decision_reader());
  return authored;
}

[[nodiscard]] inline auto make_configured_reviewer_executor(std::string name)
    -> wh::core::result<wh::agent::reviewer_executor> {
  wh::agent::reviewer_executor authored{std::move(name)};
  auto reviewer = make_executable_agent("reviewer");
  if (reviewer.has_error()) {
    return wh::core::result<wh::agent::reviewer_executor>::failure(
        reviewer.error());
  }
  auto executor = make_executable_agent("executor");
  if (executor.has_error()) {
    return wh::core::result<wh::agent::reviewer_executor>::failure(
        executor.error());
  }
  auto reviewer_set = authored.set_reviewer(std::move(reviewer).value());
  if (reviewer_set.has_error()) {
    return wh::core::result<wh::agent::reviewer_executor>::failure(
        reviewer_set.error());
  }
  auto executor_set = authored.set_executor(std::move(executor).value());
  if (executor_set.has_error()) {
    return wh::core::result<wh::agent::reviewer_executor>::failure(
        executor_set.error());
  }
  authored.set_executor_request_builder(make_revision_request_builder());
  authored.set_reviewer_request_builder(make_revision_request_builder());
  authored.set_review_decision_reader(
      make_review_decision_reader(wh::agent::review_decision_kind::accept));
  return authored;
}

[[nodiscard]] inline auto make_configured_self_refine(std::string name)
    -> wh::core::result<wh::agent::self_refine> {
  wh::agent::self_refine authored{std::move(name)};
  auto worker = make_executable_agent("worker");
  if (worker.has_error()) {
    return wh::core::result<wh::agent::self_refine>::failure(worker.error());
  }
  auto worker_set = authored.set_worker(std::move(worker).value());
  if (worker_set.has_error()) {
    return wh::core::result<wh::agent::self_refine>::failure(worker_set.error());
  }
  authored.set_worker_request_builder(make_revision_request_builder());
  authored.set_reviewer_request_builder(make_revision_request_builder());
  authored.set_review_decision_reader(
      make_review_decision_reader(wh::agent::review_decision_kind::accept));
  return authored;
}

[[nodiscard]] inline auto make_configured_reflexion(std::string name)
    -> wh::core::result<wh::agent::reflexion> {
  wh::agent::reflexion authored{std::move(name)};
  auto actor = make_executable_agent("actor");
  if (actor.has_error()) {
    return wh::core::result<wh::agent::reflexion>::failure(actor.error());
  }
  auto critic = make_executable_agent("critic");
  if (critic.has_error()) {
    return wh::core::result<wh::agent::reflexion>::failure(critic.error());
  }
  auto actor_set = authored.set_actor(std::move(actor).value());
  if (actor_set.has_error()) {
    return wh::core::result<wh::agent::reflexion>::failure(actor_set.error());
  }
  auto critic_set = authored.set_critic(std::move(critic).value());
  if (critic_set.has_error()) {
    return wh::core::result<wh::agent::reflexion>::failure(critic_set.error());
  }
  authored.set_actor_request_builder(make_revision_request_builder());
  authored.set_critic_request_builder(make_revision_request_builder());
  authored.set_review_decision_reader(
      make_review_decision_reader(wh::agent::review_decision_kind::accept));
  return authored;
}

[[nodiscard]] inline auto make_configured_research(std::string name)
    -> wh::core::result<wh::agent::research> {
  wh::agent::research authored{name};
  auto lead = make_executable_agent(name);
  if (lead.has_error()) {
    return wh::core::result<wh::agent::research>::failure(lead.error());
  }
  auto specialist = make_executable_agent("specialist");
  if (specialist.has_error()) {
    return wh::core::result<wh::agent::research>::failure(specialist.error());
  }
  auto lead_set = authored.set_lead(std::move(lead).value());
  if (lead_set.has_error()) {
    return wh::core::result<wh::agent::research>::failure(lead_set.error());
  }
  auto specialist_set = authored.add_specialist(std::move(specialist).value());
  if (specialist_set.has_error()) {
    return wh::core::result<wh::agent::research>::failure(
        specialist_set.error());
  }
  return authored;
}

[[nodiscard]] inline auto make_configured_supervisor(std::string name)
    -> wh::core::result<wh::agent::supervisor> {
  wh::agent::supervisor authored{name};
  auto supervisor = make_executable_agent(name);
  if (supervisor.has_error()) {
    return wh::core::result<wh::agent::supervisor>::failure(supervisor.error());
  }
  auto worker = make_executable_agent("worker");
  if (worker.has_error()) {
    return wh::core::result<wh::agent::supervisor>::failure(worker.error());
  }
  auto supervisor_set = authored.set_supervisor(std::move(supervisor).value());
  if (supervisor_set.has_error()) {
    return wh::core::result<wh::agent::supervisor>::failure(
        supervisor_set.error());
  }
  auto worker_set = authored.add_worker(std::move(worker).value());
  if (worker_set.has_error()) {
    return wh::core::result<wh::agent::supervisor>::failure(worker_set.error());
  }
  return authored;
}

[[nodiscard]] inline auto make_configured_swarm(std::string name)
    -> wh::core::result<wh::agent::swarm> {
  wh::agent::swarm authored{name};
  auto host = make_executable_agent(name);
  if (host.has_error()) {
    return wh::core::result<wh::agent::swarm>::failure(host.error());
  }
  auto peer = make_executable_agent("peer");
  if (peer.has_error()) {
    return wh::core::result<wh::agent::swarm>::failure(peer.error());
  }
  auto host_set = authored.set_host(std::move(host).value());
  if (host_set.has_error()) {
    return wh::core::result<wh::agent::swarm>::failure(host_set.error());
  }
  auto peer_set = authored.add_peer(std::move(peer).value());
  if (peer_set.has_error()) {
    return wh::core::result<wh::agent::swarm>::failure(peer_set.error());
  }
  return authored;
}

} // namespace wh::testing::helper
