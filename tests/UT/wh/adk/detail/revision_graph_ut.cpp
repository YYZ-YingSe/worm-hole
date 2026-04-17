#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>
#include <vector>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/detail/revision_graph.hpp"

namespace {

[[nodiscard]] auto run_pre(wh::compose::graph_add_node_options &options,
                           wh::compose::graph_process_state &process_state,
                           wh::compose::graph_value &payload)
    -> wh::core::result<void> {
  REQUIRE(static_cast<bool>(options.state.pre().handler));
  wh::compose::graph_state_cause cause{};
  wh::core::run_context context{};
  return options.state.pre().handler(cause, process_state, payload, context);
}

[[nodiscard]] auto run_post(wh::compose::graph_add_node_options &options,
                            wh::compose::graph_process_state &process_state,
                            wh::compose::graph_value &payload)
    -> wh::core::result<void> {
  REQUIRE(static_cast<bool>(options.state.post().handler));
  wh::compose::graph_state_cause cause{};
  wh::core::run_context context{};
  return options.state.post().handler(cause, process_state, payload, context);
}

} // namespace

TEST_CASE("revision detail helpers expose runtime context payload readers and shared-state lookup",
          "[UT][wh/adk/detail/revision_graph.hpp][revision_detail::make_context][condition][branch][boundary]") {
  using runtime_state = wh::adk::detail::revision_detail::runtime_state;

  runtime_state state{
      .input_messages = {wh::testing::helper::make_text_message(
          wh::schema::message_role::user, "draft")},
      .draft_history = {wh::agent::agent_output{}},
      .current_draft = wh::agent::agent_output{},
      .review_history = {wh::agent::agent_output{}},
      .current_review = wh::agent::agent_output{},
      .remaining_iterations = 2U,
  };
  auto context = wh::adk::detail::revision_detail::make_context(state);
  REQUIRE(context.input_messages.size() == 1U);
  REQUIRE(context.current_draft != nullptr);
  REQUIRE(context.current_review != nullptr);
  REQUIRE(context.draft_history.size() == 1U);
  REQUIRE(context.review_history.size() == 1U);
  REQUIRE(context.remaining_iterations == 2U);

  wh::compose::graph_value messages_payload = state.input_messages;
  auto messages =
      wh::adk::detail::revision_detail::read_messages_payload(messages_payload);
  REQUIRE(messages.has_value());
  REQUIRE(messages->size() == 1U);

  wh::compose::graph_value invalid_messages_payload = 7;
  auto invalid_messages =
      wh::adk::detail::revision_detail::read_messages_payload(
          invalid_messages_payload);
  REQUIRE(invalid_messages.has_error());
  REQUIRE(invalid_messages.error() == wh::core::errc::type_mismatch);

  wh::agent::agent_output output{};
  output.final_message = wh::testing::helper::make_text_message(
      wh::schema::message_role::assistant, "draft");
  wh::compose::graph_value output_payload = output;
  auto moved = wh::adk::detail::revision_detail::move_agent_output(output_payload);
  REQUIRE(moved.has_value());
  REQUIRE(moved->final_message.role == wh::schema::message_role::assistant);

  wh::compose::graph_value invalid_output_payload = std::monostate{};
  auto invalid_output =
      wh::adk::detail::revision_detail::move_agent_output(invalid_output_payload);
  REQUIRE(invalid_output.has_error());
  REQUIRE(invalid_output.error() == wh::core::errc::type_mismatch);

  wh::compose::graph_process_state parent{};
  REQUIRE(parent.emplace<runtime_state>(runtime_state{.remaining_iterations = 5U})
              .has_value());
  wh::compose::graph_process_state child{&parent};
  auto shared = wh::adk::detail::revision_detail::read_state(child);
  REQUIRE(shared.has_value());
  REQUIRE(shared->get().remaining_iterations == 5U);
}

TEST_CASE("revision callbacks bootstrap request capture review parse and emit final draft branches",
          "[UT][wh/adk/detail/revision_graph.hpp][revision_detail::make_bootstrap_options][condition][branch][boundary]") {
  using runtime_state = wh::adk::detail::revision_detail::runtime_state;
  using review_result = wh::adk::detail::revision_detail::review_result;

  wh::compose::graph_process_state process_state{};
  wh::compose::graph_value payload = std::vector<wh::schema::message>{
      wh::testing::helper::make_text_message(wh::schema::message_role::user,
                                             "seed"),
  };
  auto bootstrap =
      wh::adk::detail::revision_detail::make_bootstrap_options(2U);
  REQUIRE(run_pre(bootstrap, process_state, payload).has_value());
  auto state = process_state.get<runtime_state>();
  REQUIRE(state.has_value());
  REQUIRE(state->get().input_messages.size() == 1U);
  REQUIRE(state->get().remaining_iterations == 2U);

  auto request =
      wh::adk::detail::revision_detail::make_request_options(
          wh::testing::helper::make_revision_request_builder(), true);
  REQUIRE(run_pre(request, process_state, payload).has_value());
  REQUIRE(wh::core::any_cast<std::vector<wh::schema::message>>(&payload) != nullptr);
  REQUIRE(state->get().remaining_iterations == 1U);

  state->get().remaining_iterations = 0U;
  auto exhausted = run_pre(request, process_state, payload);
  REQUIRE(exhausted.has_error());
  REQUIRE(exhausted.error() == wh::core::errc::resource_exhausted);
  state->get().remaining_iterations = 1U;

  wh::agent::agent_output old_draft{};
  old_draft.final_message = wh::testing::helper::make_text_message(
      wh::schema::message_role::assistant, "old draft");
  state->get().current_draft = old_draft;

  wh::agent::agent_output new_draft{};
  new_draft.final_message = wh::testing::helper::make_text_message(
      wh::schema::message_role::assistant, "new draft");
  payload = new_draft;
  auto capture = wh::adk::detail::revision_detail::make_capture_draft_options();
  REQUIRE(run_post(capture, process_state, payload).has_value());
  REQUIRE(state->get().draft_history.size() == 1U);
  REQUIRE(state->get().current_draft.has_value());
  REQUIRE(state->get().current_draft->final_message.parts.size() == 1U);

  wh::agent::agent_output old_review{};
  old_review.final_message = wh::testing::helper::make_text_message(
      wh::schema::message_role::assistant, "old review");
  state->get().current_review = old_review;
  payload = wh::agent::agent_output{
      .final_message = wh::testing::helper::make_text_message(
          wh::schema::message_role::assistant, "review"),
  };
  auto parse_review =
      wh::adk::detail::revision_detail::make_parse_review_options(
          wh::testing::helper::make_review_decision_reader(
              wh::agent::review_decision_kind::accept));
  REQUIRE(run_post(parse_review, process_state, payload).has_value());
  REQUIRE(state->get().review_history.size() == 1U);
  auto *parsed_review = wh::core::any_cast<review_result>(&payload);
  REQUIRE(parsed_review != nullptr);
  REQUIRE(parsed_review->decision.kind == wh::agent::review_decision_kind::accept);

  wh::compose::graph_process_state empty_state{};
  payload = std::monostate{};
  auto emit_final = wh::adk::detail::revision_detail::make_emit_final_options();
  auto missing_final = run_post(emit_final, empty_state, payload);
  REQUIRE(missing_final.has_error());
  REQUIRE(missing_final.error() == wh::core::errc::not_found);

  payload = std::monostate{};
  REQUIRE(run_post(emit_final, process_state, payload).has_value());
  REQUIRE(wh::core::any_cast<wh::agent::agent_output>(&payload) != nullptr);
}

TEST_CASE("revision graph lowerer and binder surfaces support self refine reviewer executor and reflexion shells",
          "[UT][wh/adk/detail/revision_graph.hpp][bind_reflexion_agent][condition][branch][boundary]") {
  auto draft_graph =
      wh::testing::helper::make_passthrough_graph("draft_node");
  auto review_graph =
      wh::testing::helper::make_passthrough_graph("review_node");
  REQUIRE(draft_graph.has_value());
  REQUIRE(review_graph.has_value());

  wh::adk::detail::revision_detail::graph_config config{
      .graph_name = "revision",
      .max_iterations = 2U,
      .draft_request_builder = wh::testing::helper::make_revision_request_builder(),
      .review_request_builder = wh::testing::helper::make_revision_request_builder(),
      .review_decision_reader = wh::testing::helper::make_review_decision_reader(
          wh::agent::review_decision_kind::accept),
      .draft_graph = std::move(draft_graph).value(),
      .review_graph = std::move(review_graph).value(),
  };
  auto lowered = wh::adk::detail::revision_detail::lower_graph(config);
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->compile().has_value());

  auto with_memory_draft =
      wh::testing::helper::make_passthrough_graph("memory_node");
  REQUIRE(with_memory_draft.has_value());
  config.memory_request_builder = wh::testing::helper::make_revision_request_builder();
  config.memory_graph = std::move(with_memory_draft).value();
  auto lowered_with_memory =
      wh::adk::detail::revision_detail::lower_graph(std::move(config));
  REQUIRE(lowered_with_memory.has_value());
  REQUIRE(lowered_with_memory->compile().has_value());

  auto self_refine =
      wh::testing::helper::make_configured_self_refine("self-refine");
  REQUIRE(self_refine.has_value());
  REQUIRE(self_refine->freeze().has_value());
  auto bound_self_refine =
      wh::adk::detail::bind_self_refine_agent(std::move(self_refine).value());
  REQUIRE(bound_self_refine.has_value());
  REQUIRE(bound_self_refine->lower().has_value());

  auto reviewer =
      wh::testing::helper::make_configured_reviewer_executor("reviewer");
  REQUIRE(reviewer.has_value());
  REQUIRE(reviewer->freeze().has_value());
  auto bound_reviewer = wh::adk::detail::bind_reviewer_executor_agent(
      std::move(reviewer).value());
  REQUIRE(bound_reviewer.has_value());
  REQUIRE(bound_reviewer->lower().has_value());

  auto reflexion = wh::testing::helper::make_configured_reflexion("reflexion");
  REQUIRE(reflexion.has_value());
  auto memory_writer = wh::testing::helper::make_executable_agent("memory");
  REQUIRE(memory_writer.has_value());
  REQUIRE(reflexion->set_memory_writer(std::move(memory_writer).value()).has_value());
  REQUIRE(reflexion->set_memory_writer_request_builder(
              wh::testing::helper::make_revision_request_builder())
              .has_value());
  REQUIRE(reflexion->freeze().has_value());
  auto bound_reflexion =
      wh::adk::detail::bind_reflexion_agent(std::move(reflexion).value());
  REQUIRE(bound_reflexion.has_value());
  auto bound_graph = bound_reflexion->lower();
  REQUIRE(bound_graph.has_value());
  REQUIRE(bound_graph->compile().has_value());
}
