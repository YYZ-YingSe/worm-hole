#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/adk/runner.hpp"
#include "wh/core/resume_state.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

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

[[nodiscard]] auto read_messages(const wh::compose::graph_value &value)
    -> wh::core::result<std::vector<wh::schema::message>> {
  if (const auto *typed = wh::core::any_cast<std::vector<wh::schema::message>>(&value);
      typed != nullptr) {
    return *typed;
  }
  return wh::core::result<std::vector<wh::schema::message>>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto read_messages(const wh::compose::graph_input &input)
    -> wh::core::result<std::vector<wh::schema::message>> {
  const auto *payload = input.value_payload();
  if (payload == nullptr) {
    return wh::core::result<std::vector<wh::schema::message>>::failure(
        wh::core::errc::contract_violation);
  }
  return read_messages(*payload);
}

[[nodiscard]] auto make_ready_stream() -> wh::adk::agent_event_stream_reader {
  auto [writer, reader] = wh::adk::make_agent_event_stream();
  auto sent = wh::adk::send_agent_event(writer, wh::adk::make_control_event(wh::adk::control_action{
                                                    .kind = wh::adk::control_action_kind::exit,
                                                }));
  REQUIRE(sent.has_value());
  REQUIRE(wh::adk::close_agent_event_stream(writer).has_value());
  return std::move(reader);
}

[[nodiscard]] auto make_ready_output() -> wh::adk::agent_run_output {
  return wh::adk::agent_run_output{
      .events = make_ready_stream(),
  };
}

struct captured_run_state {
  std::vector<wh::schema::message> messages{};
  const wh::core::run_context *context_address{nullptr};
  bool has_checkpoint_service{false};
  std::optional<std::string> checkpoint_thread_key{};
  std::size_t resume_batch_count{0U};
  std::size_t resume_context_count{0U};
  bool reinterrupt_unmatched{false};
};

struct lowered_runner_impl {
  mutable std::optional<captured_run_state> captured{};

  [[nodiscard]] auto run(const wh::compose::graph_invoke_request &request,
                         wh::core::run_context &context) const {
    captured_run_state state{};
    auto messages = read_messages(request.input);
    REQUIRE(messages.has_value());
    state.messages = std::move(messages).value();
    state.context_address = std::addressof(context);
    state.has_checkpoint_service =
        request.services != nullptr && request.services->checkpoint.store != nullptr;
    if (request.controls.checkpoint.save.has_value()) {
      state.checkpoint_thread_key = request.controls.checkpoint.save->thread_key;
    }
    state.resume_batch_count = request.controls.resume.batch_items.size();
    state.resume_context_count = request.controls.resume.contexts.size();
    state.reinterrupt_unmatched = request.controls.resume.reinterrupt_unmatched;
    captured = std::move(state);
    return stdexec::just(wh::adk::agent_run_result{make_ready_output()});
  }
};

struct sender_runner_impl {
  mutable const wh::core::run_context *observed_context{nullptr};

  [[nodiscard]] auto run(wh::compose::graph_invoke_request &&,
                         wh::core::run_context &context) const {
    return stdexec::just() | stdexec::then([this, &context]() -> wh::adk::agent_run_result {
             observed_context = std::addressof(context);
             return make_ready_output();
           });
  }
};

} // namespace

TEST_CASE("adk runner utility helpers normalize run paths events message collection and "
          "final-message projection",
          "[UT][wh/adk/runner.hpp][append_run_path_prefix][condition][branch][boundary]") {
  const auto combined = wh::adk::append_run_path_prefix(wh::adk::run_path{{"parent", "agent"}},
                                                        wh::adk::run_path{{"child"}});
  REQUIRE(combined.to_string("/") == "parent/agent/child");

  auto prefixed = wh::adk::prefix_agent_event(
      wh::adk::make_message_event(make_assistant_message("done"),
                                  wh::adk::event_metadata{.path = wh::adk::run_path{{"leaf"}}}),
      wh::adk::run_path{{"root"}});
  REQUIRE(prefixed.metadata.path.to_string("/") == "root/leaf");

  wh::adk::agent_message_stream_reader message_reader{wh::schema::stream::make_values_stream_reader(
      std::vector<wh::schema::message>{make_user_message("a"), make_assistant_message("b")})};
  auto messages = wh::adk::collect_agent_messages(std::move(message_reader));
  REQUIRE(messages.has_value());
  REQUIRE(messages.value().size() == 2U);

  std::vector<wh::adk::agent_event> collected_event_values{};
  collected_event_values.push_back(wh::adk::make_control_event(wh::adk::control_action{
      .kind = wh::adk::control_action_kind::exit,
  }));
  collected_event_values.push_back(wh::adk::make_message_event(make_assistant_message("final")));
  wh::adk::agent_event_stream_reader event_reader{
      wh::schema::stream::make_values_stream_reader(std::move(collected_event_values))};
  auto events = wh::adk::collect_agent_events(std::move(event_reader));
  REQUIRE(events.has_value());
  REQUIRE(events.value().size() == 2U);
  auto final_message = wh::adk::find_final_message(events.value());
  REQUIRE(final_message.has_value());
  REQUIRE(std::get<wh::schema::text_part>(final_message->parts.front()).text == "final");

  wh::adk::agent_message_stream_reader nested_reader{wh::schema::stream::make_values_stream_reader(
      std::vector<wh::schema::message>{make_assistant_message("nested")})};
  std::vector<wh::adk::agent_event> mixed_events{};
  mixed_events.push_back(wh::adk::make_message_event(make_assistant_message("first")));
  mixed_events.push_back(wh::adk::make_message_event(std::move(nested_reader)));
  auto first_message = wh::adk::find_final_message(mixed_events);
  REQUIRE(first_message.has_value());
  REQUIRE(std::get<wh::schema::text_part>(first_message->parts.front()).text == "first");

  auto run_request = wh::adk::make_run_request(wh::adk::query_request{.text = "hello"});
  REQUIRE(run_request.messages.size() == 1U);
  REQUIRE(run_request.messages.front().role == wh::schema::message_role::user);
}

TEST_CASE("adk runner detail helpers lower requests and resolve resume targets from explicit and "
          "contextual locations",
          "[UT][wh/adk/runner.hpp][append_resume_targets][condition][branch][boundary]") {
  REQUIRE_FALSE(wh::adk::detail::has_checkpoint_service(nullptr));

  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;
  REQUIRE(wh::adk::detail::has_checkpoint_service(&services));

  wh::adk::run_request request{};
  request.messages.push_back(make_user_message("hello"));
  request.options.compose_services = &services;
  request.options.compose_controls.checkpoint.save =
      wh::compose::checkpoint_save_options{.thread_key = "thread-1"};

  const auto lowered_const = wh::adk::detail::lower_run_request(request);
  auto lowered_messages = read_messages(lowered_const.input);
  REQUIRE(lowered_messages.has_value());
  REQUIRE(lowered_messages.value().size() == 1U);
  REQUIRE(lowered_const.services == &services);
  REQUIRE(lowered_const.controls.checkpoint.save.has_value());

  auto lowered_move = wh::adk::detail::lower_run_request(std::move(request));
  auto moved_messages = read_messages(lowered_move.input);
  REQUIRE(moved_messages.has_value());
  REQUIRE(moved_messages.value().size() == 1U);

  wh::core::run_context context{};
  context.resume_info.emplace();
  REQUIRE(
      context.resume_info->upsert("resume-1", wh::core::address{{"graph", "a"}}, 1).has_value());
  context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "interrupt-1",
      .location = wh::core::address{{"graph", "b"}},
  };

  auto from_resume = wh::adk::detail::resolve_resume_location_one(context, "resume-1");
  REQUIRE(from_resume.has_value());
  REQUIRE(from_resume.value() == wh::core::address({"graph", "a"}));

  auto from_interrupt = wh::adk::detail::resolve_resume_location_one(context, "interrupt-1");
  REQUIRE(from_interrupt.has_value());
  REQUIRE(from_interrupt.value() == wh::core::address({"graph", "b"}));

  auto missing = wh::adk::detail::resolve_resume_location_one(context, "missing");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  auto explicit_location = wh::adk::detail::resolve_resume_location(
      context, wh::adk::resume_target{
                   .interrupt_id = "resume-1",
                   .location = wh::core::address{{"graph", "explicit"}},
                   .payload = wh::core::any{7},
               });
  REQUIRE(explicit_location.has_value());
  REQUIRE(explicit_location.value() == wh::core::address({"graph", "explicit"}));

  wh::compose::graph_invoke_controls controls{};
  auto appended = wh::adk::detail::append_resume_targets(
      context,
      std::vector<wh::adk::resume_target>{wh::adk::resume_target{
                                              .interrupt_id = "resume-1",
                                              .payload = wh::core::any{3},
                                          },
                                          wh::adk::resume_target{
                                              .interrupt_id = "interrupt-1",
                                              .payload = wh::core::any{4},
                                          }},
      true, controls);
  REQUIRE(appended.has_value());
  REQUIRE(controls.resume.reinterrupt_unmatched);
  REQUIRE(controls.resume.batch_items.size() == 2U);
  REQUIRE(controls.resume.contexts.size() == 2U);

  wh::compose::graph_invoke_controls invalid_controls{};
  auto invalid = wh::adk::detail::append_resume_targets(
      context,
      std::vector<wh::adk::resume_target>{wh::adk::resume_target{
          .interrupt_id = "",
          .payload = wh::core::any{1},
      }},
      false, invalid_controls);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
}

TEST_CASE(
    "adk runner facade lowers query run and resume requests onto the underlying implementation",
    "[UT][wh/adk/runner.hpp][runner::resume][condition][branch][boundary]") {
  lowered_runner_impl impl{};
  wh::adk::runner<lowered_runner_impl> runner{impl};
  wh::core::run_context context{};

  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;

  wh::adk::query_request query{};
  query.text = "hello runner";
  query.options.compose_services = &services;
  query.options.compose_controls.checkpoint.save =
      wh::compose::checkpoint_save_options{.thread_key = "runner-query"};
  auto query_status = wh::testing::helper::wait_value_on_test_thread(runner.query(query, context));
  REQUIRE(query_status.has_value());
  REQUIRE(runner.implementation().captured.has_value());
  REQUIRE(runner.implementation().captured->messages.size() == 1U);
  REQUIRE(runner.implementation().captured->context_address == std::addressof(context));
  REQUIRE(runner.implementation().captured->has_checkpoint_service);
  REQUIRE(runner.implementation().captured->checkpoint_thread_key ==
          std::optional<std::string>{"runner-query"});

  wh::adk::resume_request resume{};
  resume.run.messages.push_back(make_user_message("resume"));
  resume.run.options.compose_services = &services;
  resume.targets.push_back(wh::adk::resume_target{
      .interrupt_id = "interrupt-A",
      .location = wh::core::address{{"graph", "a"}},
      .payload = wh::core::any{3},
  });
  resume.targets.push_back(wh::adk::resume_target{
      .interrupt_id = "interrupt-B",
      .location = wh::core::address{{"graph", "b"}},
      .payload = wh::core::any{4},
  });
  resume.reinterrupt_unmatched = true;

  auto resume_status =
      wh::testing::helper::wait_value_on_test_thread(runner.resume(resume, context));
  REQUIRE(resume_status.has_value());
  REQUIRE(runner.implementation().captured.has_value());
  REQUIRE(runner.implementation().captured->resume_batch_count == 2U);
  REQUIRE(runner.implementation().captured->resume_context_count == 2U);
  REQUIRE(runner.implementation().captured->reinterrupt_unmatched);
}

TEST_CASE("adk runner rejects missing checkpoint services when required and preserves moved run "
          "context for sender implementations",
          "[UT][wh/adk/runner.hpp][runner::run][condition][branch][boundary]") {
  lowered_runner_impl lowered{};
  wh::adk::runner<lowered_runner_impl> lowered_runner{lowered};
  wh::core::run_context context{};

  wh::adk::resume_request request{};
  request.run.messages.push_back(make_user_message("resume"));
  request.targets.push_back(wh::adk::resume_target{
      .interrupt_id = "interrupt-A",
      .location = wh::core::address{{"graph", "a"}},
      .payload = wh::core::any{3},
  });

  auto missing_checkpoint =
      wh::testing::helper::wait_value_on_test_thread(lowered_runner.resume(request, context));
  REQUIRE(missing_checkpoint.has_error());
  REQUIRE(missing_checkpoint.error() == wh::core::errc::not_found);

  sender_runner_impl sender{};
  wh::adk::runner<sender_runner_impl> sender_runner{sender};
  wh::adk::run_request run{};
  run.messages.push_back(make_user_message("async"));
  auto run_status =
      wh::testing::helper::wait_value_on_test_thread(sender_runner.run(std::move(run), context));
  REQUIRE(run_status.has_value());
  REQUIRE(sender_runner.implementation().observed_context == std::addressof(context));
}

TEST_CASE(
    "adk runner ownerizes lvalue requests and rejects non-ownable request payloads explicitly",
    "[UT][wh/adk/runner.hpp][runner::resume][boundary]") {
  wh::core::run_context context{};

  lowered_runner_impl run_impl{};
  wh::adk::runner<lowered_runner_impl> run_runner{run_impl};
  wh::adk::run_request lvalue_run{};
  lvalue_run.messages.push_back(make_user_message("run"));
  lvalue_run.options.compose_controls.call.component_defaults.insert_or_assign(
      "move-only", wh::core::any{std::make_unique<int>(7)});

  auto lvalue_run_status =
      wh::testing::helper::wait_value_on_test_thread(run_runner.run(lvalue_run, context));
  REQUIRE(lvalue_run_status.has_error());
  REQUIRE_FALSE(run_runner.implementation().captured.has_value());

  wh::adk::run_request rvalue_run{};
  rvalue_run.messages.push_back(make_user_message("run"));
  rvalue_run.options.compose_controls.call.component_defaults.insert_or_assign(
      "move-only", wh::core::any{std::make_unique<int>(9)});
  auto rvalue_run_status = wh::testing::helper::wait_value_on_test_thread(
      run_runner.run(std::move(rvalue_run), context));
  REQUIRE(rvalue_run_status.has_value());
  REQUIRE(run_runner.implementation().captured.has_value());

  lowered_runner_impl query_impl{};
  wh::adk::runner<lowered_runner_impl> query_runner{query_impl};
  wh::adk::query_request lvalue_query{};
  lvalue_query.text = "query";
  lvalue_query.options.compose_controls.call.component_defaults.insert_or_assign(
      "move-only", wh::core::any{std::make_unique<int>(11)});
  auto lvalue_query_status =
      wh::testing::helper::wait_value_on_test_thread(query_runner.query(lvalue_query, context));
  REQUIRE(lvalue_query_status.has_error());
  REQUIRE_FALSE(query_runner.implementation().captured.has_value());

  wh::adk::query_request rvalue_query{};
  rvalue_query.text = "query";
  rvalue_query.options.compose_controls.call.component_defaults.insert_or_assign(
      "move-only", wh::core::any{std::make_unique<int>(13)});
  auto rvalue_query_status = wh::testing::helper::wait_value_on_test_thread(
      query_runner.query(std::move(rvalue_query), context));
  REQUIRE(rvalue_query_status.has_value());
  REQUIRE(query_runner.implementation().captured.has_value());
  REQUIRE(query_runner.implementation().captured->messages.size() == 1U);

  lowered_runner_impl resume_impl{};
  wh::adk::runner<lowered_runner_impl> resume_runner{resume_impl};
  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;

  wh::adk::resume_request lvalue_resume{};
  lvalue_resume.run.messages.push_back(make_user_message("resume"));
  lvalue_resume.run.options.compose_services = &services;
  lvalue_resume.targets.push_back(wh::adk::resume_target{
      .interrupt_id = "resume-move-only",
      .location = wh::core::address{{"graph", "a"}},
      .payload = wh::core::any{std::make_unique<int>(3)},
  });
  auto lvalue_resume_status =
      wh::testing::helper::wait_value_on_test_thread(resume_runner.resume(lvalue_resume, context));
  REQUIRE(lvalue_resume_status.has_error());
  REQUIRE_FALSE(resume_runner.implementation().captured.has_value());

  wh::adk::resume_request rvalue_resume{};
  rvalue_resume.run.messages.push_back(make_user_message("resume"));
  rvalue_resume.run.options.compose_services = &services;
  rvalue_resume.targets.push_back(wh::adk::resume_target{
      .interrupt_id = "resume-move-only",
      .location = wh::core::address{{"graph", "a"}},
      .payload = wh::core::any{std::make_unique<int>(5)},
  });
  auto rvalue_resume_status = wh::testing::helper::wait_value_on_test_thread(
      resume_runner.resume(std::move(rvalue_resume), context));
  REQUIRE(rvalue_resume_status.has_value());
  REQUIRE(resume_runner.implementation().captured.has_value());
  REQUIRE(resume_runner.implementation().captured->resume_batch_count == 1U);
}
