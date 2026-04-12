#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/adk/runner.hpp"
#include "wh/compose/graph/invoke_types.hpp"
#include "wh/core/run_context.hpp"

namespace {

[[nodiscard]] auto make_user_message(const std::string &text)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

[[nodiscard]] auto read_messages(const wh::compose::graph_value &value)
    -> wh::core::result<std::vector<wh::schema::message>> {
  if (const auto *typed =
          wh::core::any_cast<std::vector<wh::schema::message>>(&value);
      typed != nullptr) {
    return *typed;
  }
  return wh::core::result<std::vector<wh::schema::message>>::failure(
      wh::core::errc::type_mismatch);
}

[[nodiscard]] auto make_ready_stream() -> wh::adk::agent_event_stream_reader {
  auto [writer, reader] = wh::adk::make_agent_event_stream();
  auto sent = wh::adk::send_agent_event(
      writer, wh::adk::make_control_event(
                  wh::adk::control_action{
                      .kind = wh::adk::control_action_kind::exit,
                  }));
  REQUIRE(sent.has_value());
  REQUIRE(wh::adk::close_agent_event_stream(writer).has_value());
  return std::move(reader);
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

[[nodiscard]] auto make_ready_output() -> wh::adk::agent_run_output {
  return wh::adk::agent_run_output{
      .events = make_ready_stream(),
  };
}

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
        request.services != nullptr &&
        request.services->checkpoint.store != nullptr;

    if (request.controls.checkpoint.save.has_value()) {
      state.checkpoint_thread_key =
          request.controls.checkpoint.save->thread_key;
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
    return stdexec::just() |
           stdexec::then([this, &context]() -> wh::adk::agent_run_result {
             observed_context = std::addressof(context);
             return make_ready_output();
           });
  }
};

} // namespace

TEST_CASE("adk runner query wraps one user message and lowers typed invoke options",
          "[core][adk][condition]") {
  lowered_runner_impl impl{};
  wh::adk::runner<lowered_runner_impl> runner{impl};
  wh::core::run_context parent{};

  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;

  wh::adk::query_request request{};
  request.text = "hello runner";
  request.options.compose_services = &services;
  request.options.compose_controls.checkpoint.save =
      wh::compose::checkpoint_save_options{
          .thread_key = "runner-query",
      };

  auto started =
      wh::testing::helper::wait_value_on_test_thread(runner.query(request, parent));
  REQUIRE(started.has_value());
  REQUIRE(runner.implementation().captured.has_value());
  REQUIRE(runner.implementation().captured->messages.size() == 1U);
  REQUIRE(runner.implementation().captured->messages.front().role ==
          wh::schema::message_role::user);
  REQUIRE(std::get<wh::schema::text_part>(
              runner.implementation().captured->messages.front().parts.front())
              .text == "hello runner");
  REQUIRE(runner.implementation().captured->context_address ==
          std::addressof(parent));
  REQUIRE(runner.implementation().captured->has_checkpoint_service);
  REQUIRE(runner.implementation().captured->checkpoint_thread_key ==
          std::optional<std::string>{"runner-query"});
}

TEST_CASE("adk runner forwards the same run context on fresh runs",
          "[core][adk][boundary]") {
  lowered_runner_impl impl{};
  wh::adk::runner<lowered_runner_impl> runner{impl};
  wh::core::run_context parent{};

  wh::adk::run_request request{};
  request.messages.push_back(make_user_message("isolated"));

  auto started =
      wh::testing::helper::wait_value_on_test_thread(runner.run(request, parent));
  REQUIRE(started.has_value());
  REQUIRE(runner.implementation().captured.has_value());
  REQUIRE(runner.implementation().captured->context_address ==
          std::addressof(parent));
}

TEST_CASE("adk runner resume lowers explicit targets into compose resume controls",
          "[core][adk][condition]") {
  lowered_runner_impl impl{};
  wh::adk::runner<lowered_runner_impl> runner{impl};
  wh::core::run_context parent{};

  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;

  wh::adk::resume_request request{};
  request.run.messages.push_back(make_user_message("resume"));
  request.run.options.compose_services = &services;
  request.targets.push_back(wh::adk::resume_target{
      .interrupt_id = "interrupt-A",
      .location = wh::core::address{{"graph", "a"}},
      .payload = wh::core::any{3},
  });
  request.targets.push_back(wh::adk::resume_target{
      .interrupt_id = "interrupt-B",
      .location = wh::core::address{{"graph", "b"}},
      .payload = wh::core::any{4},
  });
  request.reinterrupt_unmatched = true;

  auto started = wh::testing::helper::wait_value_on_test_thread(
      runner.resume(request, parent));
  REQUIRE(started.has_value());
  REQUIRE(runner.implementation().captured.has_value());
  REQUIRE(runner.implementation().captured->has_checkpoint_service);
  REQUIRE(runner.implementation().captured->resume_batch_count == 2U);
  REQUIRE(runner.implementation().captured->resume_context_count == 2U);
  REQUIRE(runner.implementation().captured->reinterrupt_unmatched);
}

TEST_CASE("adk runner empty target resume degenerates to implicit resume controls",
          "[core][adk][boundary]") {
  lowered_runner_impl impl{};
  wh::adk::runner<lowered_runner_impl> runner{impl};
  wh::core::run_context parent{};

  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;

  wh::adk::resume_request request{};
  request.run.messages.push_back(make_user_message("resume"));
  request.run.options.compose_services = &services;
  request.reinterrupt_unmatched = true;

  auto started = wh::testing::helper::wait_value_on_test_thread(
      runner.resume(request, parent));
  REQUIRE(started.has_value());
  REQUIRE(runner.implementation().captured.has_value());
  REQUIRE(runner.implementation().captured->resume_batch_count == 0U);
  REQUIRE(runner.implementation().captured->resume_context_count == 0U);
  REQUIRE(runner.implementation().captured->reinterrupt_unmatched);
}

TEST_CASE("adk runner resume fails fast without checkpoint services when required",
          "[core][adk][boundary]") {
  lowered_runner_impl impl{};
  wh::adk::runner<lowered_runner_impl> runner{impl};
  wh::core::run_context parent{};

  wh::adk::resume_request request{};
  request.run.messages.push_back(make_user_message("resume"));
  request.targets.push_back(wh::adk::resume_target{
      .interrupt_id = "interrupt-A",
      .location = wh::core::address{{"graph", "a"}},
      .payload = wh::core::any{3},
  });

  auto started = wh::testing::helper::wait_value_on_test_thread(
      runner.resume(request, parent));
  REQUIRE(started.has_error());
  REQUIRE(started.error() == wh::core::errc::not_found);
}

TEST_CASE("adk runner keeps the forwarded run context alive across sender-based implementations",
          "[core][adk][condition]") {
  sender_runner_impl impl{};
  wh::adk::runner<sender_runner_impl> runner{impl};
  wh::core::run_context parent{};

  wh::adk::run_request request{};
  request.messages.push_back(make_user_message("async"));

  auto started = wh::testing::helper::wait_value_on_test_thread(
      runner.run(std::move(request), parent));
  REQUIRE(started.has_value());
  REQUIRE(runner.implementation().observed_context == std::addressof(parent));
}
