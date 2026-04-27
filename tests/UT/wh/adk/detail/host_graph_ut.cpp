#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/detail/host_graph.hpp"

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

[[nodiscard]] auto make_definition() -> wh::adk::detail::host_graph_detail::host_graph_definition {
  auto root_graph = wh::testing::helper::make_passthrough_graph("root_node");
  auto child_graph = wh::testing::helper::make_passthrough_graph("child_node");
  REQUIRE(root_graph.has_value());
  REQUIRE(child_graph.has_value());

  return wh::adk::detail::host_graph_detail::host_graph_definition{
      .root =
          wh::adk::detail::host_graph_detail::host_member_definition{
              .name = "root",
              .description = "root desc",
              .allow_transfer_to_parent = false,
              .allowed_children = {"worker"},
              .graph = std::move(root_graph).value(),
          },
      .children = {wh::adk::detail::host_graph_detail::host_member_definition{
          .name = "worker",
          .description = "worker desc",
          .parent_name = "root",
          .allow_transfer_to_parent = true,
          .graph = std::move(child_graph).value(),
      }},
  };
}

} // namespace

TEST_CASE(
    "host graph detail helpers compare messages normalize history and resolve transfer targets",
    "[UT][wh/adk/detail/"
    "host_graph.hpp][host_graph_detail::normalize_history_delta][condition][branch][boundary]") {
  using namespace wh::adk::detail::host_graph_detail;

  wh::schema::message text_message{};
  text_message.role = wh::schema::message_role::assistant;
  text_message.parts.emplace_back(wh::schema::text_part{"alpha"});
  wh::schema::message same_text = text_message;
  REQUIRE(equal_message_part(text_message.parts.front(), same_text.parts.front()));

  wh::schema::message different{};
  different.role = wh::schema::message_role::assistant;
  different.parts.emplace_back(wh::schema::text_part{"beta"});
  REQUIRE_FALSE(equal_message(text_message, different));
  REQUIRE(equal_message(text_message, same_text));

  wh::schema::message empty{};
  REQUIRE_FALSE(message_has_payload(empty));
  REQUIRE(message_has_payload(text_message));

  stamp_agent_message(text_message, "worker");
  REQUIRE(text_message.name == "worker");
  wh::schema::message user_message =
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "ask");
  stamp_agent_message(user_message, "worker");
  REQUIRE(user_message.name.empty());

  REQUIRE(matches_history_prefix({same_text, different}, {same_text}));
  REQUIRE_FALSE(matches_history_prefix({same_text}, {same_text, different}));

  wh::agent::agent_output output{};
  output.final_message =
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "done");
  output.history_messages = {same_text, output.final_message};
  auto delta = normalize_history_delta(output, {same_text}, "worker");
  REQUIRE(delta.size() == 1U);
  REQUIRE(delta.front().name == "worker");

  wh::agent::agent_output fallback_output{};
  fallback_output.final_message =
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "fallback");
  auto fallback_delta = normalize_history_delta(fallback_output, {}, "worker");
  REQUIRE(fallback_delta.size() == 1U);
  REQUIRE(fallback_delta.front().name == "worker");

  auto transfer_assistant = wh::adk::make_transfer_assistant_message("worker", "call-1");
  auto transfer_tool = wh::adk::make_transfer_tool_message("worker", "call-1");
  auto filtered = filter_transfer_messages(
      {transfer_assistant, transfer_tool, fallback_output.final_message}, "call-1");
  REQUIRE(filtered.size() == 1U);

  auto transfer_state = make_transfer_state("root");
  REQUIRE(transfer_state.visited_agents.contains("root"));
  REQUIRE(member_node_key("worker") == "agent/worker");

  auto definition = make_definition();
  auto root = member_ref(definition, "root");
  auto child = member_ref(definition, "worker");
  REQUIRE(root.has_value());
  REQUIRE(child.has_value());
  REQUIRE_FALSE(has_member(definition, "ghost"));

  auto current_target = resolve_transfer_target(definition, root->get(), "root");
  REQUIRE(current_target.has_value());
  REQUIRE(current_target->kind == wh::adk::transfer_target_kind::current);

  auto child_target = resolve_transfer_target(definition, root->get(), "worker");
  REQUIRE(child_target.has_value());
  REQUIRE(child_target->kind == wh::adk::transfer_target_kind::child);

  auto parent_target = resolve_transfer_target(definition, child->get(), "root");
  REQUIRE(parent_target.has_value());
  REQUIRE(parent_target->kind == wh::adk::transfer_target_kind::parent);

  auto forbidden = resolve_transfer_target(definition, root->get(), "ghost");
  REQUIRE(forbidden.has_error());
  REQUIRE(forbidden.error() == wh::core::errc::not_found);
}

TEST_CASE("host graph topology helpers validate member graphs export placeholders and capture "
          "final output",
          "[UT][wh/adk/detail/"
          "host_graph.hpp][host_graph_detail::build_definition][condition][branch][boundary]") {
  using namespace wh::adk::detail::host_graph_detail;

  auto valid_definition = make_definition();
  runtime_state host_state{};
  auto built_output =
      build_final_output(host_state,
                         wh::agent::agent_output{
                             .final_message = wh::testing::helper::make_text_message(
                                 wh::schema::message_role::assistant, "done"),
                             .history_messages = {wh::testing::helper::make_text_message(
                                 wh::schema::message_role::assistant, "done")},
                         },
                         "worker");
  REQUIRE(built_output.final_message.name == "worker");
  REQUIRE_FALSE(built_output.transfer.has_value());

  auto value_value = wh::testing::helper::make_executable_message_agent(
      "vv", wh::compose::node_contract::value, wh::compose::node_contract::value,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "vv"));
  auto value_stream = wh::testing::helper::make_executable_message_agent(
      "vs", wh::compose::node_contract::value, wh::compose::node_contract::stream,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "vs"));
  auto stream_value = wh::testing::helper::make_executable_message_agent(
      "sv", wh::compose::node_contract::stream, wh::compose::node_contract::value,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "sv"));
  auto stream_stream = wh::testing::helper::make_executable_message_agent(
      "ss", wh::compose::node_contract::stream, wh::compose::node_contract::stream,
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "ss"));
  REQUIRE(value_value.has_value());
  REQUIRE(value_stream.has_value());
  REQUIRE(stream_value.has_value());
  REQUIRE(stream_stream.has_value());

  auto vv_definition =
      lower_member_definition(wh::agent::make_role_binding(std::move(value_value).value()));
  auto vs_definition =
      lower_member_definition(wh::agent::make_role_binding(std::move(value_stream).value()));
  auto sv_definition =
      lower_member_definition(wh::agent::make_role_binding(std::move(stream_value).value()));
  auto ss_definition =
      lower_member_definition(wh::agent::make_role_binding(std::move(stream_stream).value()));
  REQUIRE(vv_definition.has_value());
  REQUIRE(vs_definition.has_value());
  REQUIRE(sv_definition.has_value());
  REQUIRE(ss_definition.has_value());
  REQUIRE(vv_definition->graph.boundary().input == wh::compose::node_contract::value);
  REQUIRE(vv_definition->graph.boundary().output == wh::compose::node_contract::value);
  REQUIRE(vs_definition->graph.boundary().input == wh::compose::node_contract::value);
  REQUIRE(vs_definition->graph.boundary().output == wh::compose::node_contract::stream);
  REQUIRE(sv_definition->graph.boundary().input == wh::compose::node_contract::stream);
  REQUIRE(sv_definition->graph.boundary().output == wh::compose::node_contract::value);
  REQUIRE(ss_definition->graph.boundary().input == wh::compose::node_contract::stream);
  REQUIRE(ss_definition->graph.boundary().output == wh::compose::node_contract::stream);

  auto root = wh::testing::helper::make_executable_agent("root");
  auto worker = wh::testing::helper::make_executable_agent("worker");
  REQUIRE(root.has_value());
  REQUIRE(worker.has_value());
  auto root_binding = wh::agent::make_role_binding(std::move(root).value());
  std::vector<wh::agent::role_binding> children{};
  children.push_back(wh::agent::make_role_binding(std::move(worker).value()));
  auto built = build_definition(root_binding, children);
  REQUIRE(built.has_value());
  REQUIRE(built->get()->children.size() == 1U);
  REQUIRE(built->get()->root.allowed_children == std::vector<std::string>{"worker"});

  auto exported = populate_exported_topology(*built->get());
  REQUIRE(exported.has_value());
  REQUIRE(exported->allows_transfer_to_child("worker"));
  auto exported_child = exported->child("worker");
  REQUIRE(exported_child.has_value());
  REQUIRE(exported_child->get().allows_transfer_to_parent());
}

TEST_CASE(
    "host graph state callbacks lowerers and binders cover request routing transfer and final "
    "branches",
    "[UT][wh/adk/detail/host_graph.hpp][bind_supervisor_agent][condition][branch][boundary]") {
  using namespace wh::adk::detail::host_graph_detail;

  auto definition = make_definition();
  wh::compose::graph_process_state process_state{};
  wh::compose::graph_value payload = std::vector<wh::schema::message>{
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "seed"),
  };
  auto bootstrap = make_bootstrap_options(definition);
  REQUIRE(run_pre(bootstrap, process_state, payload).has_value());
  auto state = process_state.workflow_state_ref<runtime_state>();
  REQUIRE(state.has_value());
  REQUIRE(state->get().visible_history.size() == 1U);
  REQUIRE(state->get().active_agent_name == "root");

  auto prepare = make_prepare_request_options();
  REQUIRE(run_pre(prepare, process_state, payload).has_value());
  auto *host_request =
      wh::core::any_cast<wh::adk::detail::host_graph_detail::host_request>(&payload);
  REQUIRE(host_request != nullptr);
  REQUIRE(host_request->agent_name == "root");

  auto role_value = project_role_request_to_value(payload, "root");
  REQUIRE(role_value.has_value());
  REQUIRE(wh::core::any_cast<std::vector<wh::schema::message>>(&role_value.value()) != nullptr);

  payload = wh::adk::detail::host_graph_detail::host_request{
      .agent_name = "root",
      .messages = {wh::testing::helper::make_text_message(wh::schema::message_role::user, "seed")},
  };
  auto role_stream = project_role_request_to_stream(payload, "root");
  REQUIRE(role_stream.has_value());
  auto streamed_messages = wh::adk::detail::read_message_stream(std::move(role_stream).value());
  REQUIRE(streamed_messages.has_value());
  REQUIRE(streamed_messages->size() == 1U);

  payload = wh::adk::detail::host_graph_detail::host_request{
      .agent_name = "other",
      .messages = {},
  };
  auto wrong_role = project_role_request_to_value(payload, "root");
  REQUIRE(wrong_role.has_error());
  REQUIRE(wrong_role.error() == wh::core::errc::contract_violation);

  state->get().active_agent_name = "root";
  state->get().active_request_messages = {
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "seed"),
  };
  wh::agent::agent_output transfer_output{};
  transfer_output.final_message = wh::adk::make_transfer_tool_message("worker", "call-1");
  transfer_output.history_messages = {
      wh::adk::make_transfer_assistant_message("worker", "call-1"),
      wh::adk::make_transfer_tool_message("worker", "call-1"),
  };
  transfer_output.transfer = wh::agent::agent_transfer{
      .target_agent_name = "worker",
      .tool_call_id = "call-1",
  };
  payload = transfer_output;
  auto capture = make_capture_options(definition);
  REQUIRE(run_post(capture, process_state, payload).has_value());
  auto *invoke_next = wh::core::any_cast<host_step_decision>(&payload);
  REQUIRE(invoke_next != nullptr);
  REQUIRE(invoke_next->kind == host_step_kind::invoke_next);
  REQUIRE(state->get().active_agent_name == "worker");

  state->get().active_request_messages = {
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "worker request"),
  };
  wh::agent::agent_output final_output{};
  final_output.final_message =
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "done");
  final_output.history_messages.push_back(final_output.final_message);
  payload = final_output;
  REQUIRE(run_post(capture, process_state, payload).has_value());
  auto *emit_final = wh::core::any_cast<host_step_decision>(&payload);
  REQUIRE(emit_final != nullptr);
  REQUIRE(emit_final->kind == host_step_kind::emit_final);
  REQUIRE(state->get().final_output.has_value());

  auto emit_options = make_emit_final_options();
  REQUIRE(run_pre(emit_options, process_state, payload).has_value());
  REQUIRE(wh::core::any_cast<wh::agent::agent_output>(&payload) != nullptr);

  auto lowered = host_graph{definition}.lower();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->compiled());

  auto supervisor = wh::testing::helper::make_configured_supervisor("supervisor");
  REQUIRE(supervisor.has_value());
  REQUIRE(supervisor->freeze().has_value());
  auto bound_supervisor = wh::adk::detail::bind_supervisor_agent(std::move(supervisor).value());
  REQUIRE(bound_supervisor.has_value());
  REQUIRE(bound_supervisor->lower().has_value());

  auto swarm = wh::testing::helper::make_configured_swarm("swarm");
  REQUIRE(swarm.has_value());
  REQUIRE(swarm->freeze().has_value());
  auto bound_swarm = wh::adk::detail::bind_swarm_agent(std::move(swarm).value());
  REQUIRE(bound_swarm.has_value());
  REQUIRE(bound_swarm->lower().has_value());

  auto research = wh::testing::helper::make_configured_research("research");
  REQUIRE(research.has_value());
  REQUIRE(research->freeze().has_value());
  auto bound_research = wh::adk::detail::bind_research_agent(std::move(research).value());
  REQUIRE(bound_research.has_value());
  REQUIRE(bound_research->lower().has_value());
}
