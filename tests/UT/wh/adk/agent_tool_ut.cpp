#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "wh/adk/agent_tool.hpp"
#include "wh/adk/detail/history_request.hpp"

namespace {

[[nodiscard]] auto make_user_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

[[nodiscard]] auto make_assistant_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

[[nodiscard]] auto message_text(const wh::schema::message &message) -> std::string {
  return std::get<wh::schema::text_part>(message.parts.front()).text;
}

[[nodiscard]] auto collect_events(wh::adk::agent_event_stream_reader &reader)
    -> std::vector<wh::adk::agent_event> {
  std::vector<wh::adk::agent_event> events{};
  while (true) {
    auto next = reader.read();
    REQUIRE(next.has_value());
    REQUIRE_FALSE(next.value().error.failed());
    if (next.value().eof) {
      break;
    }
    REQUIRE(next.value().value.has_value());
    events.push_back(std::move(*next.value().value));
  }
  return events;
}

[[nodiscard]] auto read_graph_string(wh::compose::graph_value &&value) -> std::string {
  auto *typed = wh::core::any_cast<std::string>(&value);
  REQUIRE(typed != nullptr);
  return std::move(*typed);
}

[[nodiscard]] auto read_graph_stream_text(wh::compose::graph_stream_reader &reader)
    -> std::vector<std::string> {
  std::vector<std::string> chunks{};
  while (true) {
    auto next = reader.read();
    REQUIRE(next.has_value());
    REQUIRE_FALSE(next.value().error.failed());
    if (next.value().eof) {
      break;
    }
    REQUIRE(next.value().value.has_value());
    auto *typed = wh::core::any_cast<std::string>(&*next.value().value);
    REQUIRE(typed != nullptr);
    chunks.push_back(std::move(*typed));
  }
  return chunks;
}

struct scripted_agent_tool_runner_state {
  std::vector<wh::adk::run_request> seen_requests{};
  std::vector<std::string> message_texts{};
  std::vector<std::string> resumed_message_texts{};
  bool emit_transfer{false};
  bool emit_interrupt{false};
  bool emit_error{false};
  bool omit_final_message{false};
  bool add_child_metadata{false};
  bool set_interrupt_context{false};
  std::size_t resume_projection_count{0U};
  std::optional<wh::compose::interrupt_decision_kind> expected_resume_decision{};
  std::string child_interrupt_id{"child-interrupt"};
  wh::core::address child_interrupt_location{{"agent", "worker", "tool", "leaf", "call-1"}};
  wh::core::any child_interrupt_state{std::string{"child-state"}};
  wh::core::any child_interrupt_payload{std::string{"child-payload"}};
};

[[nodiscard]] auto run_scripted_agent_tool(scripted_agent_tool_runner_state &state,
                                           const wh::adk::run_request &request,
                                           wh::core::run_context &context)
    -> wh::adk::agent_run_result {
  state.seen_requests.push_back(request);

  bool resumed = false;
  const auto &resume = request.options.compose_controls.resume;
  if (resume.decision.has_value()) {
    resumed = true;
    ++state.resume_projection_count;
  }

  const auto &message_texts = resumed ? state.resumed_message_texts : state.message_texts;
  std::vector<wh::adk::agent_event> events{};
  for (const auto &text : message_texts) {
    wh::adk::event_metadata metadata{};
    if (state.add_child_metadata) {
      metadata.path = wh::adk::run_path{{"agent", "leaf"}};
      metadata.agent_name = "leaf";
    }
    events.push_back(
        wh::adk::make_message_event(make_assistant_message(text), std::move(metadata)));
  }
  if (state.emit_transfer) {
    events.push_back(wh::adk::make_control_event(wh::adk::control_action{
        .kind = wh::adk::control_action_kind::transfer,
        .target = "other-agent",
    }));
  }
  if (state.emit_interrupt) {
    events.push_back(wh::adk::make_control_event(
        wh::adk::control_action{
            .kind = wh::adk::control_action_kind::interrupt,
            .interrupt_id = "interrupt-1",
        },
        wh::adk::event_metadata{
            .path = wh::adk::run_path{{"agent", "leaf"}},
            .agent_name = "leaf",
        }));
  }
  if (state.emit_error) {
    events.push_back(wh::adk::make_error_event(wh::core::make_error(wh::core::errc::unavailable),
                                               "child failed"));
  }

  if (state.emit_interrupt && state.set_interrupt_context) {
    context.interrupt_info = wh::core::interrupt_context{
        .interrupt_id = state.child_interrupt_id,
        .location = state.child_interrupt_location,
        .state = state.child_interrupt_state,
        .layer_payload = state.child_interrupt_payload,
        .trigger_reason = "child interrupt",
    };
  }

  std::optional<wh::schema::message> final_message{};
  if (!state.omit_final_message && !message_texts.empty()) {
    final_message = make_assistant_message(message_texts.back());
  }

  return wh::adk::agent_run_output{
      .events = wh::adk::agent_event_stream_reader{wh::schema::stream::make_values_stream_reader(
          std::move(events))},
      .final_message = std::move(final_message),
  };
}

[[nodiscard]] auto make_call_scope(wh::core::run_context &context, const std::string_view tool_name,
                                   const std::string_view call_id) -> wh::tool::call_scope {
  return wh::tool::call_scope{
      .run = context,
      .component = "agent_tool_ut",
      .implementation = "agent_tool_ut",
      .tool_name = tool_name,
      .call_id = call_id,
  };
}

struct object_runner {
  [[nodiscard]] auto run(const wh::adk::run_request &request, wh::core::run_context &)
      -> wh::adk::agent_run_result {
    std::vector<wh::adk::agent_event> events{};
    events.push_back(wh::adk::make_message_event(
        make_assistant_message(message_text(request.messages.front()))));
    return wh::adk::agent_run_output{
        .events = wh::adk::agent_event_stream_reader{wh::schema::stream::make_values_stream_reader(
            std::move(events))},
    };
  }
};

static_assert(wh::adk::detail::agent_tool_runner_object<object_runner>);
static_assert(!wh::adk::detail::agent_tool_runner_callable<object_runner>);
static_assert(wh::adk::detail::bindable_agent_tool_runner<object_runner>);
static_assert(wh::adk::detail::agent_tool_runner_callable<
              decltype([](const wh::adk::run_request &, wh::core::run_context &)
                           -> wh::adk::agent_run_result { return wh::adk::agent_run_output{}; })>);

} // namespace

TEST_CASE("agent tool surface validates metadata schema freeze and runner dispatch helpers",
          "[UT][wh/adk/agent_tool.hpp][agent_tool::freeze][condition][branch][boundary]") {
  REQUIRE(wh::adk::agent_tool_input_mode::request == wh::adk::agent_tool_input_mode{0U});

  wh::adk::agent_tool_result empty_result{};
  REQUIRE(empty_result.output_chunks.empty());
  REQUIRE(empty_result.output_text.empty());
  REQUIRE_FALSE(empty_result.final_message.has_value());
  REQUIRE_FALSE(empty_result.final_error.has_value());
  REQUIRE_FALSE(empty_result.interrupted);

  wh::adk::run_request request{};
  request.messages.push_back(make_user_message("dispatch"));
  wh::core::run_context context{};
  object_runner runner_object{};
  auto dispatched = wh::adk::detail::dispatch_agent_tool_runner(runner_object, request, context);
  REQUIRE(dispatched.has_value());

  wh::adk::agent_tool tool{"delegate", "delegate request", wh::agent::agent{"worker"}};
  REQUIRE(tool.name() == "delegate");
  REQUIRE(tool.description() == "delegate request");
  REQUIRE(tool.input_mode() == wh::adk::agent_tool_input_mode::request);
  REQUIRE_FALSE(tool.frozen());
  REQUIRE_FALSE(tool.forward_internal_events());
  REQUIRE(tool.bound_agent().name() == "worker");

  REQUIRE(tool.set_input_mode(wh::adk::agent_tool_input_mode::custom_schema).has_value());
  auto missing_custom = tool.freeze();
  REQUIRE(missing_custom.has_error());
  REQUIRE(missing_custom.error() == wh::core::errc::invalid_argument);

  wh::schema::tool_schema_definition custom_schema{};
  custom_schema.parameters.push_back(wh::schema::tool_parameter_schema{
      .name = "count",
      .type = wh::schema::tool_parameter_type::integer,
      .description = "work count",
      .required = true,
  });
  REQUIRE(tool.set_custom_schema(std::move(custom_schema)).has_value());
  REQUIRE(tool.set_forward_internal_events(true).has_value());
  REQUIRE(tool.forward_internal_events());
  REQUIRE(tool.bind_runner(wh::adk::detail::agent_tool_runner{nullptr}).has_error());
  REQUIRE(tool.bind_runner(object_runner{}).has_value());
  REQUIRE(tool.freeze().has_value());
  auto resolved_schema = tool.tool_schema();
  REQUIRE(resolved_schema.parameters.size() == 1U);
  REQUIRE(resolved_schema.parameters.front().name == "count");

  REQUIRE(tool.set_forward_internal_events(false).has_error());
  REQUIRE(tool.set_custom_schema({}).has_error());
  REQUIRE(tool.bind_runner(object_runner{}).has_error());

  wh::adk::agent_tool missing_runner_tool{"missing", "missing runner", wh::agent::agent{"worker"}};
  auto missing_runner_freeze = missing_runner_tool.freeze();
  REQUIRE(missing_runner_freeze.has_error());
  REQUIRE(missing_runner_freeze.error() == wh::core::errc::not_found);
}

TEST_CASE("agent tool request history and entry paths map inputs flatten outputs and preserve "
          "boundary events",
          "[UT][wh/adk/"
          "agent_tool.hpp][agent_tool::compose_entry][condition][branch][boundary][concurrency]") {
  wh::adk::agent_tool request_tool{"delegate", "delegate request", wh::agent::agent{"worker"}};
  auto runner_state =
      std::make_shared<scripted_agent_tool_runner_state>(scripted_agent_tool_runner_state{
          .message_texts = {"first", "second"}, .add_child_metadata = true});
  REQUIRE(
      request_tool
          .bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());

  wh::compose::tool_call request_call{
      .call_id = "call-1",
      .tool_name = "delegate",
      .arguments = R"({"request":"hello bridge"})",
  };
  wh::core::run_context request_context{};
  auto unfrozen_request_result = request_tool.run(
      request_call, make_call_scope(request_context, request_call.tool_name, request_call.call_id));
  REQUIRE(unfrozen_request_result.has_error());
  REQUIRE(unfrozen_request_result.error() == wh::core::errc::contract_violation);
  REQUIRE(request_tool.freeze().has_value());
  auto request_result = request_tool.run(
      request_call, make_call_scope(request_context, request_call.tool_name, request_call.call_id));
  REQUIRE(request_result.has_value());
  REQUIRE(runner_state->seen_requests.size() == 1U);
  REQUIRE(message_text(runner_state->seen_requests.front().messages.front()) == "hello bridge");
  REQUIRE(request_result.value().output_chunks == std::vector<std::string>{"first", "second"});
  REQUIRE(request_result.value().output_text == "firstsecond");
  auto event_reader = std::move(request_result).value().events;
  auto events = collect_events(event_reader);
  REQUIRE(events.size() == 1U);
  REQUIRE(events.front().metadata.path.to_string("/") == "tool/delegate/call-1/agent/worker");

  wh::adk::agent_tool history_tool{"delegate_history", "delegate history",
                                   wh::agent::agent{"worker"}};
  REQUIRE(history_tool.set_input_mode(wh::adk::agent_tool_input_mode::message_history).has_value());
  REQUIRE(
      history_tool
          .bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  wh::adk::detail::history_request_payload payload{};
  payload.history_request.messages.push_back(make_user_message("keep structured user"));
  payload.state_payload = wh::core::any(std::string{"bridge-state"});
  wh::compose::tool_call history_call{
      .call_id = "call-2",
      .tool_name = "delegate_history",
      .arguments = "{}",
      .payload = wh::core::any(std::move(payload)),
  };
  wh::core::run_context history_context{};
  auto unfrozen_history_result = history_tool.run(
      history_call, make_call_scope(history_context, history_call.tool_name, history_call.call_id));
  REQUIRE(unfrozen_history_result.has_error());
  REQUIRE(unfrozen_history_result.error() == wh::core::errc::contract_violation);
  REQUIRE(history_tool.freeze().has_value());
  auto history_result = history_tool.run(
      history_call, make_call_scope(history_context, history_call.tool_name, history_call.call_id));
  REQUIRE(history_result.has_value());
  REQUIRE(runner_state->seen_requests.size() >= 2U);
  REQUIRE(message_text(runner_state->seen_requests.back().messages.front()) ==
          "keep structured user");

  auto entry = request_tool.compose_entry();
  REQUIRE(entry.has_value());
  auto invoke_status = entry.value().invoke(
      request_call, make_call_scope(request_context, request_call.tool_name, request_call.call_id));
  REQUIRE(invoke_status.has_value());
  REQUIRE(read_graph_string(std::move(invoke_status).value()) == "firstsecond");
  auto stream_status = entry.value().stream(
      request_call, make_call_scope(request_context, request_call.tool_name, request_call.call_id));
  REQUIRE(stream_status.has_value());
  auto chunks = read_graph_stream_text(stream_status.value());
  REQUIRE(chunks == std::vector<std::string>{"first", "second"});

  auto [message_writer, message_reader] =
      wh::schema::stream::make_pipe_stream<wh::schema::message>(4U);
  auto child_reader = std::make_shared<std::optional<wh::adk::agent_message_stream_reader>>(
      wh::adk::agent_message_stream_reader{std::move(message_reader)});
  wh::adk::agent_tool live_tool{"delegate_live", "delegate live", wh::agent::agent{"worker"}};
  auto bind_live_runner =
      live_tool.bind_runner([child_reader](const wh::adk::run_request &request,
                                           wh::core::run_context &) -> wh::adk::agent_run_result {
        REQUIRE(message_text(request.messages.front()) == "live please");
        std::vector<wh::adk::agent_event> live_events{};
        live_events.push_back(wh::adk::make_message_event(
            std::move(**child_reader), wh::adk::event_metadata{
                                           .path = wh::adk::run_path{{"agent", "worker"}},
                                       }));
        child_reader->reset();
        return wh::adk::agent_run_output{
            .events =
                wh::adk::agent_event_stream_reader{
                    wh::schema::stream::make_values_stream_reader(std::move(live_events))},
        };
      });
  REQUIRE(bind_live_runner.has_value());
  auto unfrozen_live_entry = live_tool.compose_entry();
  REQUIRE(unfrozen_live_entry.has_error());
  REQUIRE(unfrozen_live_entry.error() == wh::core::errc::contract_violation);
  REQUIRE(live_tool.freeze().has_value());
  auto live_entry = live_tool.compose_entry();
  REQUIRE(live_entry.has_value());
  wh::compose::tool_call live_call{
      .call_id = "call-live",
      .tool_name = "delegate_live",
      .arguments = R"({"request":"live please"})",
  };
  wh::core::run_context live_context{};
  std::atomic<bool> produced{false};
  std::atomic<bool> write_succeeded{false};
  std::atomic<bool> close_succeeded{false};
  std::thread producer{[&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    write_succeeded.store(
        message_writer.try_write(make_assistant_message("late chunk")).has_value(),
        std::memory_order_release);
    close_succeeded.store(message_writer.close().has_value(), std::memory_order_release);
    produced.store(true, std::memory_order_release);
  }};
  auto live_stream = live_entry.value().stream(
      live_call, make_call_scope(live_context, live_call.tool_name, live_call.call_id));
  REQUIRE(live_stream.has_value());
  REQUIRE_FALSE(produced.load(std::memory_order_acquire));
  {
    stdexec::inplace_stop_source stop_source{};
    wh::testing::helper::sender_capture<> completion{};
    auto operation = stdexec::connect(live_stream.value().read_async(),
                                      wh::testing::helper::sender_capture_receiver{
                                          &completion,
                                          wh::testing::helper::make_scheduler_env(
                                              stdexec::inline_scheduler{}, stop_source.get_token()),
                                      });
    stdexec::start(operation);
    stop_source.request_stop();
    REQUIRE(completion.ready.try_acquire_for(std::chrono::milliseconds(100)));
    REQUIRE(completion.terminal == wh::testing::helper::sender_terminal_kind::stopped);
  }
  producer.join();
  REQUIRE(write_succeeded.load(std::memory_order_acquire));
  REQUIRE(close_succeeded.load(std::memory_order_acquire));
  auto resumed = live_stream.value().read();
  REQUIRE(resumed.has_value());
  REQUIRE(resumed.value().value.has_value());
  REQUIRE(read_graph_string(std::move(*resumed.value().value)) == "late chunk");
}

TEST_CASE("agent tool filters controls reports protocol and interrupt outcomes and exposes mode "
          "specific schemas",
          "[UT][wh/adk/agent_tool.hpp][agent_tool::tool_schema][condition][branch][boundary]") {
  wh::adk::agent_tool history_schema_tool{"history", "history tool", wh::agent::agent{"worker"}};
  REQUIRE(history_schema_tool.set_input_mode(wh::adk::agent_tool_input_mode::message_history)
              .has_value());
  auto history_schema = history_schema_tool.tool_schema();
  REQUIRE(history_schema.raw_parameters_json_schema.find("messages") != std::string::npos);

  wh::adk::agent_tool request_schema_tool{"request", "request tool", wh::agent::agent{"worker"}};
  auto request_schema = request_schema_tool.tool_schema();
  REQUIRE(request_schema.parameters.size() == 1U);
  REQUIRE(request_schema.parameters.front().name == "request");

  wh::adk::agent_tool empty_tool{"empty", "empty tool", wh::agent::agent{"worker"}};
  auto empty_state = std::make_shared<scripted_agent_tool_runner_state>(
      scripted_agent_tool_runner_state{.emit_transfer = true, .omit_final_message = true});
  REQUIRE(
      empty_tool
          .bind_runner([empty_state](const wh::adk::run_request &request,
                                     wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*empty_state, request, context);
          })
          .has_value());
  REQUIRE(empty_tool.freeze().has_value());
  wh::compose::tool_call empty_call{
      .call_id = "call-empty",
      .tool_name = "empty",
      .arguments = R"({"request":"hello"})",
  };
  wh::core::run_context empty_context{};
  auto empty_result = empty_tool.run(
      empty_call, make_call_scope(empty_context, empty_call.tool_name, empty_call.call_id));
  REQUIRE(empty_result.has_error());
  REQUIRE(empty_result.error() == wh::core::errc::protocol_error);

  wh::adk::agent_tool interrupt_tool{"interrupt", "interrupt tool", wh::agent::agent{"worker"}};
  auto interrupt_state =
      std::make_shared<scripted_agent_tool_runner_state>(scripted_agent_tool_runner_state{
          .message_texts = {"draft"},
          .emit_interrupt = true,
          .set_interrupt_context = true,
      });
  REQUIRE(interrupt_tool
              .bind_runner(
                  [interrupt_state](const wh::adk::run_request &request,
                                    wh::core::run_context &context) -> wh::adk::agent_run_result {
                    return run_scripted_agent_tool(*interrupt_state, request, context);
                  })
              .has_value());
  REQUIRE(interrupt_tool.freeze().has_value());
  wh::compose::tool_call interrupt_call{
      .call_id = "call-interrupt",
      .tool_name = "interrupt",
      .arguments = R"({"request":"resume me"})",
  };
  wh::core::run_context interrupt_context{};
  auto interrupted = interrupt_tool.run(
      interrupt_call,
      make_call_scope(interrupt_context, interrupt_call.tool_name, interrupt_call.call_id));
  REQUIRE(interrupted.has_value());
  REQUIRE(interrupted.value().interrupted);
  REQUIRE(interrupt_context.interrupt_info.has_value());
}
