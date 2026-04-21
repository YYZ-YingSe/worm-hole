#include <atomic>
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
#include "wh/compose/graph.hpp"
#include "wh/compose/node/compiled.hpp"
#include "wh/schema/stream.hpp"
#include "wh/schema/stream/reader.hpp"

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
    REQUIRE(resume.decision->interrupt_context_id == state.child_interrupt_id);
    REQUIRE(resume.contexts.size() == 1U);
    REQUIRE(resume.contexts.front().interrupt_id == state.child_interrupt_id);
    REQUIRE(resume.contexts.front().location == state.child_interrupt_location);
    if (state.expected_resume_decision.has_value()) {
      REQUIRE(resume.decision->decision == *state.expected_resume_decision);
    }
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
      .component = "agent_tool_test",
      .implementation = "agent_tool_test_impl",
      .tool_name = tool_name,
      .call_id = call_id,
  };
}

} // namespace

TEST_CASE("agent tool request mode maps request json to child chat request and "
          "aggregates text",
          "[core][agent][condition]") {
  wh::adk::agent_tool tool{"delegate", "delegate request", wh::agent::agent{"worker"}};
  auto runner_state = std::make_shared<scripted_agent_tool_runner_state>(
      scripted_agent_tool_runner_state{.message_texts = {"bridge result"}});
  REQUIRE(
      tool.bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  REQUIRE(tool.freeze().has_value());

  wh::compose::tool_call call{
      .call_id = "call-1",
      .tool_name = "delegate",
      .arguments = R"({"request":"hello bridge"})",
  };
  wh::core::run_context context{};
  auto result = tool.run(call, make_call_scope(context, call.tool_name, call.call_id));
  REQUIRE(result.has_value());
  REQUIRE(runner_state->seen_requests.size() == 1U);
  REQUIRE(runner_state->seen_requests.front().messages.size() == 1U);
  REQUIRE(message_text(runner_state->seen_requests.front().messages.front()) == "hello bridge");
  REQUIRE(result.value().output_chunks == std::vector<std::string>{"bridge result"});
  REQUIRE(result.value().output_text == "bridge result");

  auto reader = std::move(result).value().events;
  auto events = collect_events(reader);
  REQUIRE(events.size() == 1U);
  REQUIRE(events.front().metadata.path.to_string("/") == "tool/delegate/call-1/agent/worker");
}

TEST_CASE("agent tool message history mode reads projected react state and "
          "strips trailing assistant tool call",
          "[core][agent][condition]") {
  wh::adk::agent_tool tool{"delegate_history", "delegate history", wh::agent::agent{"worker"}};
  REQUIRE(tool.set_input_mode(wh::adk::agent_tool_input_mode::message_history).has_value());
  auto runner_state = std::make_shared<scripted_agent_tool_runner_state>(
      scripted_agent_tool_runner_state{.message_texts = {"history result"}});
  REQUIRE(
      tool.bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  REQUIRE(tool.freeze().has_value());

  wh::core::run_context context{};

  wh::compose::tool_call call{
      .call_id = "call-2",
      .tool_name = "delegate_history",
      .arguments = "{}",
      .payload = wh::core::any(wh::model::chat_request{
          .messages = {make_user_message("keep user")},
      }),
  };
  auto result = tool.run(call, make_call_scope(context, call.tool_name, call.call_id));
  REQUIRE(result.has_value());
  REQUIRE(runner_state->seen_requests.size() == 1U);
  REQUIRE(runner_state->seen_requests.front().messages.size() == 1U);
  REQUIRE(message_text(runner_state->seen_requests.front().messages.front()) == "keep user");
}

TEST_CASE("agent tool message history mode accepts shared history-request payload",
          "[core][agent][condition]") {
  wh::adk::agent_tool tool{"delegate_payload", "delegate payload", wh::agent::agent{"worker"}};
  REQUIRE(tool.set_input_mode(wh::adk::agent_tool_input_mode::message_history).has_value());
  auto runner_state = std::make_shared<scripted_agent_tool_runner_state>(
      scripted_agent_tool_runner_state{.message_texts = {"history result"}});
  REQUIRE(
      tool.bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  REQUIRE(tool.freeze().has_value());

  wh::core::run_context context{};
  wh::adk::detail::history_request_payload payload{};
  payload.history_request.messages.push_back(make_user_message("keep structured user"));
  payload.state_payload = wh::core::any(std::string{"bridge-state"});

  wh::compose::tool_call call{
      .call_id = "call-2b",
      .tool_name = "delegate_payload",
      .arguments = "{}",
      .payload = wh::core::any(std::move(payload)),
  };
  auto result = tool.run(call, make_call_scope(context, call.tool_name, call.call_id));
  REQUIRE(result.has_value());
  REQUIRE(runner_state->seen_requests.size() == 1U);
  REQUIRE(runner_state->seen_requests.front().messages.size() == 1U);
  REQUIRE(message_text(runner_state->seen_requests.front().messages.front()) ==
          "keep structured user");
}

TEST_CASE("agent tool flattens child message substreams without rebuilding a "
          "second event vector",
          "[core][agent][condition]") {
  wh::adk::agent_tool tool{"delegate_stream", "delegate stream", wh::agent::agent{"worker"}};
  REQUIRE(tool.set_forward_internal_events(true).has_value());
  auto bind_stream_runner =
      tool.bind_runner([](const wh::adk::run_request &request,
                          wh::core::run_context &) -> wh::adk::agent_run_result {
        REQUIRE(request.messages.size() == 1U);
        REQUIRE(message_text(request.messages.front()) == "stream please");

        std::vector<wh::schema::message> streamed_messages{};
        streamed_messages.push_back(make_assistant_message("first"));
        streamed_messages.push_back(make_assistant_message("second"));

        std::vector<wh::adk::agent_event> events{};
        events.push_back(wh::adk::make_message_event(
            wh::adk::agent_message_stream_reader{
                wh::schema::stream::make_values_stream_reader(std::move(streamed_messages))},
            wh::adk::event_metadata{
                .path = wh::adk::run_path{{"agent", "worker"}},
                .agent_name = "worker",
            }));

        return wh::adk::agent_run_output{
            .events =
                wh::adk::agent_event_stream_reader{
                    wh::schema::stream::make_values_stream_reader(std::move(events))},
            .final_message = make_assistant_message("second"),
        };
      });
  REQUIRE(bind_stream_runner.has_value());
  REQUIRE(tool.freeze().has_value());

  wh::compose::tool_call call{
      .call_id = "call-stream-1",
      .tool_name = "delegate_stream",
      .arguments = R"({"request":"stream please"})",
  };
  wh::core::run_context context{};
  auto result = tool.run(call, make_call_scope(context, call.tool_name, call.call_id));
  REQUIRE(result.has_value());
  REQUIRE(result.value().output_chunks == std::vector<std::string>{"first", "second"});
  REQUIRE(result.value().output_text == "firstsecond");

  auto reader = std::move(result).value().events;
  auto events = collect_events(reader);
  REQUIRE(events.size() == 2U);
  REQUIRE(std::holds_alternative<wh::adk::message_event>(events[0].payload));
  REQUIRE(std::holds_alternative<wh::adk::message_event>(events[1].payload));
  REQUIRE(message_text(std::get<wh::schema::message>(
              std::get<wh::adk::message_event>(events[0].payload).content)) == "first");
  REQUIRE(message_text(std::get<wh::schema::message>(
              std::get<wh::adk::message_event>(events[1].payload).content)) == "second");
}

TEST_CASE("agent tool boundary consumes non interrupt controls and fails when "
          "no boundary event remains",
          "[core][agent][boundary]") {
  wh::adk::agent_tool filtered{"delegate", "delegate request", wh::agent::agent{"worker"}};
  auto success_state =
      std::make_shared<scripted_agent_tool_runner_state>(scripted_agent_tool_runner_state{
          .message_texts = {"visible result"},
          .emit_transfer = true,
      });
  REQUIRE(filtered
              .bind_runner(
                  [success_state](const wh::adk::run_request &request,
                                  wh::core::run_context &context) -> wh::adk::agent_run_result {
                    return run_scripted_agent_tool(*success_state, request, context);
                  })
              .has_value());
  REQUIRE(filtered.freeze().has_value());

  wh::compose::tool_call success_call{
      .call_id = "call-3",
      .tool_name = "delegate",
      .arguments = R"({"request":"hello"})",
  };
  wh::core::run_context success_context{};
  auto success = filtered.run(
      success_call, make_call_scope(success_context, success_call.tool_name, success_call.call_id));
  REQUIRE(success.has_value());
  auto success_reader = std::move(success).value().events;
  auto success_events = collect_events(success_reader);
  REQUIRE(success_events.size() == 1U);
  REQUIRE_FALSE(std::holds_alternative<wh::adk::control_action>(success_events.front().payload));

  wh::adk::agent_tool empty{"delegate_empty", "delegate request", wh::agent::agent{"worker"}};
  auto empty_state = std::make_shared<scripted_agent_tool_runner_state>(
      scripted_agent_tool_runner_state{.emit_transfer = true, .omit_final_message = true});
  REQUIRE(
      empty
          .bind_runner([empty_state](const wh::adk::run_request &request,
                                     wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*empty_state, request, context);
          })
          .has_value());
  REQUIRE(empty.freeze().has_value());

  wh::compose::tool_call empty_call{
      .call_id = "call-4",
      .tool_name = "delegate_empty",
      .arguments = R"({"request":"hello"})",
  };
  wh::core::run_context empty_context{};
  auto empty_result = empty.run(
      empty_call, make_call_scope(empty_context, empty_call.tool_name, empty_call.call_id));
  REQUIRE(empty_result.has_error());
  REQUIRE(empty_result.error() == wh::core::errc::protocol_error);
}

TEST_CASE("agent tool forwards internal events with prefixed tool run path",
          "[core][agent][condition]") {
  wh::adk::agent_tool tool{"delegate_stream", "delegate stream", wh::agent::agent{"worker"}};
  REQUIRE(tool.set_forward_internal_events(true).has_value());
  auto runner_state =
      std::make_shared<scripted_agent_tool_runner_state>(scripted_agent_tool_runner_state{
          .message_texts = {"chunk-a", "chunk-b"},
          .add_child_metadata = true,
      });
  REQUIRE(
      tool.bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  REQUIRE(tool.freeze().has_value());

  wh::compose::tool_call call{
      .call_id = "call-5",
      .tool_name = "delegate_stream",
      .arguments = R"({"request":"hello"})",
  };
  wh::core::run_context context{};
  auto result = tool.run(call, make_call_scope(context, call.tool_name, call.call_id));
  REQUIRE(result.has_value());
  auto reader = std::move(result).value().events;
  auto events = collect_events(reader);
  REQUIRE(events.size() == 2U);
  REQUIRE(events.front().metadata.path.to_string("/") == "tool/delegate_stream/call-5/agent/leaf");
  REQUIRE(events.back().metadata.path.to_string("/") == "tool/delegate_stream/call-5/agent/leaf");
}

TEST_CASE("agent tool compose entry reuses same bridge for invoke and stream",
          "[core][agent][condition]") {
  wh::adk::agent_tool tool{"delegate_entry", "delegate entry", wh::agent::agent{"worker"}};
  REQUIRE(tool.set_forward_internal_events(true).has_value());
  auto runner_state =
      std::make_shared<scripted_agent_tool_runner_state>(scripted_agent_tool_runner_state{
          .message_texts = {"first", "second"},
          .add_child_metadata = true,
      });
  REQUIRE(
      tool.bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  REQUIRE(tool.freeze().has_value());

  auto entry = tool.compose_entry();
  REQUIRE(entry.has_value());

  wh::compose::tool_call call{
      .call_id = "call-6",
      .tool_name = "delegate_entry",
      .arguments = R"({"request":"invoke me"})",
  };
  wh::core::run_context context{};
  auto scope = make_call_scope(context, call.tool_name, call.call_id);

  auto invoke_status = entry.value().invoke(call, scope);
  REQUIRE(invoke_status.has_value());
  REQUIRE(read_graph_string(std::move(invoke_status).value()) == "firstsecond");

  auto stream_status = entry.value().stream(call, scope);
  REQUIRE(stream_status.has_value());
  auto reader = std::move(stream_status).value();
  auto chunks = read_graph_stream_text(reader);
  REQUIRE(chunks == std::vector<std::string>{"first", "second"});
}

TEST_CASE("agent tool compose entry stream flattens child message substreams",
          "[core][agent][condition]") {
  wh::adk::agent_tool tool{"delegate_entry_stream", "delegate entry stream",
                           wh::agent::agent{"worker"}};
  REQUIRE(tool.set_forward_internal_events(true).has_value());
  auto bind_entry_stream_runner =
      tool.bind_runner([](const wh::adk::run_request &request,
                          wh::core::run_context &) -> wh::adk::agent_run_result {
        REQUIRE(request.messages.size() == 1U);
        REQUIRE(message_text(request.messages.front()) == "stream through entry");

        std::vector<wh::schema::message> streamed_messages{};
        streamed_messages.push_back(make_assistant_message("first"));
        streamed_messages.push_back(make_assistant_message("second"));

        std::vector<wh::adk::agent_event> events{};
        events.push_back(wh::adk::make_message_event(
            wh::adk::agent_message_stream_reader{
                wh::schema::stream::make_values_stream_reader(std::move(streamed_messages))},
            wh::adk::event_metadata{
                .path = wh::adk::run_path{{"agent", "worker"}},
                .agent_name = "worker",
            }));

        return wh::adk::agent_run_output{
            .events =
                wh::adk::agent_event_stream_reader{
                    wh::schema::stream::make_values_stream_reader(std::move(events))},
            .final_message = make_assistant_message("second"),
        };
      });
  REQUIRE(bind_entry_stream_runner.has_value());
  REQUIRE(tool.freeze().has_value());

  auto entry = tool.compose_entry();
  REQUIRE(entry.has_value());

  wh::compose::tool_call call{
      .call_id = "call-entry-stream-1",
      .tool_name = "delegate_entry_stream",
      .arguments = R"({"request":"stream through entry"})",
  };
  wh::core::run_context context{};
  auto scope = make_call_scope(context, call.tool_name, call.call_id);

  auto invoke_status = entry.value().invoke(call, scope);
  REQUIRE(invoke_status.has_value());
  REQUIRE(read_graph_string(std::move(invoke_status).value()) == "firstsecond");

  auto stream_status = entry.value().stream(call, scope);
  REQUIRE(stream_status.has_value());
  auto reader = std::move(stream_status).value();
  auto chunks = read_graph_stream_text(reader);
  REQUIRE(chunks == std::vector<std::string>{"first", "second"});
}

TEST_CASE("agent tool compose entry stream returns live reader before child "
          "message stream produces data and preserves stop state",
          "[core][agent][stream][stop][condition]") {
  auto [message_writer, message_reader] =
      wh::schema::stream::make_pipe_stream<wh::schema::message>(4U);

  auto child_reader = std::make_shared<std::optional<wh::adk::agent_message_stream_reader>>(
      wh::adk::agent_message_stream_reader{std::move(message_reader)});

  wh::adk::agent_tool tool{"delegate_entry_live", "delegate entry live",
                           wh::agent::agent{"worker"}};
  auto bind_live_entry_runner =
      tool.bind_runner([child_reader](const wh::adk::run_request &request,
                                      wh::core::run_context &) -> wh::adk::agent_run_result {
        REQUIRE(request.messages.size() == 1U);
        REQUIRE(message_text(request.messages.front()) == "live please");
        REQUIRE(child_reader->has_value());

        std::vector<wh::adk::agent_event> events{};
        events.push_back(wh::adk::make_message_event(
            std::move(**child_reader), wh::adk::event_metadata{
                                           .path = wh::adk::run_path{{"agent", "worker"}},
                                           .agent_name = "worker",
                                       }));
        child_reader->reset();

        return wh::adk::agent_run_output{
            .events =
                wh::adk::agent_event_stream_reader{
                    wh::schema::stream::make_values_stream_reader(std::move(events))},
        };
      });
  REQUIRE(bind_live_entry_runner.has_value());
  REQUIRE(tool.freeze().has_value());

  auto entry = tool.compose_entry();
  REQUIRE(entry.has_value());

  wh::compose::tool_call call{
      .call_id = "call-entry-live-1",
      .tool_name = "delegate_entry_live",
      .arguments = R"({"request":"live please"})",
  };
  wh::core::run_context context{};
  auto scope = make_call_scope(context, call.tool_name, call.call_id);

  std::atomic<bool> produced{false};
  std::atomic<bool> write_succeeded{false};
  std::atomic<bool> close_succeeded{false};
  std::thread producer{[&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    write_succeeded.store(
        message_writer.try_write(make_assistant_message("late chunk")).has_value(),
        std::memory_order_release);
    close_succeeded.store(message_writer.close().has_value(), std::memory_order_release);
    produced.store(true, std::memory_order_release);
  }};

  auto stream_status = entry.value().stream(call, scope);
  REQUIRE(stream_status.has_value());
  REQUIRE_FALSE(produced.load(std::memory_order_acquire));

  auto reader = std::move(stream_status).value();

  {
    stdexec::inplace_stop_source stop_source{};
    wh::testing::helper::sender_capture<> completion{};
    auto operation = stdexec::connect(reader.read_async(),
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

  auto resumed = reader.read();
  REQUIRE(resumed.has_value());
  REQUIRE_FALSE(resumed.value().eof);
  REQUIRE_FALSE(resumed.value().error.failed());
  REQUIRE(resumed.value().value.has_value());
  REQUIRE(read_graph_string(std::move(*resumed.value().value)) == "late chunk");

  auto eof = reader.read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}

TEST_CASE("agent tool resume reuses bridge checkpoint state without "
          "duplicating prior text",
          "[core][agent][checkpoint][condition]") {
  wh::adk::agent_tool tool{"delegate_resume", "delegate resume", wh::agent::agent{"worker"}};
  auto runner_state =
      std::make_shared<scripted_agent_tool_runner_state>(scripted_agent_tool_runner_state{
          .message_texts = {"draft"},
          .resumed_message_texts = {" rest"},
          .emit_interrupt = true,
          .set_interrupt_context = true,
          .expected_resume_decision = wh::compose::interrupt_decision_kind::approve,
      });
  REQUIRE(
      tool.bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  REQUIRE(tool.freeze().has_value());

  wh::compose::tool_call call{
      .call_id = "call-resume-1",
      .tool_name = "delegate_resume",
      .arguments = R"({"request":"resume me"})",
  };

  wh::core::run_context first_context{};
  auto first = tool.run(call, make_call_scope(first_context, call.tool_name, call.call_id));
  REQUIRE(first.has_value());
  REQUIRE(first.value().interrupted);
  REQUIRE(first.value().output_text == "draft");
  REQUIRE(first_context.interrupt_info.has_value());
  REQUIRE(first_context.interrupt_info->location.to_string("/") ==
          "tool/delegate_resume/call-resume-1");
  auto *stored_state = wh::core::any_cast<wh::adk::detail::agent_tool_interrupt_state>(
      &first_context.interrupt_info->state);
  REQUIRE(stored_state != nullptr);
  REQUIRE(stored_state->checkpoint.output_chunks == std::vector<std::string>{"draft"});
  REQUIRE(stored_state->child_interrupt.has_value());
  REQUIRE(stored_state->child_interrupt->interrupt_id == runner_state->child_interrupt_id);

  const auto saved_interrupt = *first_context.interrupt_info;

  auto run_resumed =
      [&](wh::core::run_context &context) -> wh::core::result<wh::adk::agent_tool_result> {
    context.interrupt_info = saved_interrupt;
    context.resume_info.emplace();
    REQUIRE(wh::compose::add_resume_target(
                *context.resume_info, saved_interrupt.interrupt_id, saved_interrupt.location,
                wh::compose::resume_patch{
                    .decision = wh::compose::interrupt_decision_kind::approve,
                })
                .has_value());
    return tool.run(call, make_call_scope(context, call.tool_name, call.call_id));
  };

  runner_state->emit_interrupt = false;

  wh::core::run_context resumed_context{};
  auto resumed = run_resumed(resumed_context);
  REQUIRE(resumed.has_value());
  REQUIRE_FALSE(resumed.value().interrupted);
  REQUIRE(resumed.value().output_text == "draft rest");
  REQUIRE(runner_state->resume_projection_count == 1U);
  REQUIRE_FALSE(resumed_context.interrupt_info.has_value());

  wh::core::run_context replay_context{};
  auto replayed = run_resumed(replay_context);
  REQUIRE(replayed.has_value());
  REQUIRE(replayed.value().output_text == "draft rest");
  REQUIRE(runner_state->resume_projection_count == 2U);
}

TEST_CASE("agent tool exact resume target fails when bridge checkpoint state "
          "is missing",
          "[core][agent][checkpoint][boundary]") {
  wh::adk::agent_tool tool{"delegate_resume", "delegate resume", wh::agent::agent{"worker"}};
  auto runner_state = std::make_shared<scripted_agent_tool_runner_state>(
      scripted_agent_tool_runner_state{.resumed_message_texts = {"rest"}});
  REQUIRE(
      tool.bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  REQUIRE(tool.freeze().has_value());

  wh::compose::tool_call call{
      .call_id = "call-resume-missing",
      .tool_name = "delegate_resume",
      .arguments = R"({"request":"resume me"})",
  };

  wh::core::run_context context{};
  context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "outer-interrupt",
      .location = wh::core::address{"tool", "delegate_resume", "call-resume-missing"},
      .state = wh::core::any(std::string{"wrong-state"}),
  };
  context.resume_info.emplace();
  REQUIRE(wh::compose::add_resume_target(
              *context.resume_info, "outer-interrupt",
              wh::core::address{"tool", "delegate_resume", "call-resume-missing"},
              wh::compose::resume_patch{
                  .decision = wh::compose::interrupt_decision_kind::approve,
              })
              .has_value());

  auto resumed = tool.run(call, make_call_scope(context, call.tool_name, call.call_id));
  REQUIRE(resumed.has_error());
  REQUIRE(resumed.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("agent tool compose tools node projects bridge interrupt context "
          "back to node runtime",
          "[core][agent][checkpoint][condition]") {
  wh::adk::agent_tool bridge{"delegate_graph_resume", "delegate graph resume",
                             wh::agent::agent{"worker"}};
  auto runner_state =
      std::make_shared<scripted_agent_tool_runner_state>(scripted_agent_tool_runner_state{
          .message_texts = {"draft"},
          .emit_interrupt = true,
          .set_interrupt_context = true,
      });
  REQUIRE(
      bridge
          .bind_runner([runner_state](const wh::adk::run_request &request,
                                      wh::core::run_context &context) -> wh::adk::agent_run_result {
            return run_scripted_agent_tool(*runner_state, request, context);
          })
          .has_value());
  REQUIRE(bridge.freeze().has_value());
  auto entry = bridge.compose_entry();
  REQUIRE(entry.has_value());

  wh::compose::tool_registry tools{};
  tools.insert_or_assign("delegate_graph_resume", entry.value());

  wh::compose::graph graph{};
  REQUIRE(graph.add_tools(wh::compose::make_tools_node("tools", std::move(tools))).has_value());
  REQUIRE(graph.add_entry_edge("tools").has_value());
  REQUIRE(graph.add_exit_edge("tools").has_value());
  REQUIRE(graph.compile().has_value());
  auto compiled = graph.compiled_node_by_key("tools");
  REQUIRE(compiled.has_value());

  wh::compose::tool_call call{
      .call_id = "call-graph-resume",
      .tool_name = "delegate_graph_resume",
      .arguments = R"({"request":"resume graph"})",
  };

  wh::compose::graph_value input =
      wh::core::any(wh::compose::tool_batch{.calls = std::vector<wh::compose::tool_call>{call}});
  wh::core::run_context context{};
  auto status = wh::compose::run_compiled_sync_node(compiled.value().get(), input, context,
                                                    wh::compose::node_runtime{});
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::canceled);
  REQUIRE(context.interrupt_info.has_value());
  REQUIRE(context.interrupt_info->location.to_string("/") ==
          "tool/delegate_graph_resume/call-graph-resume");
  auto *stored_state = wh::core::any_cast<wh::adk::detail::agent_tool_interrupt_state>(
      &context.interrupt_info->state);
  REQUIRE(stored_state != nullptr);
  REQUIRE(stored_state->checkpoint.output_chunks == std::vector<std::string>{"draft"});
  REQUIRE(stored_state->child_interrupt.has_value());
  REQUIRE(stored_state->child_interrupt->interrupt_id == runner_state->child_interrupt_id);
}
