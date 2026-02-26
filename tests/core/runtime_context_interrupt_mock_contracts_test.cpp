#include <catch2/catch_test_macros.hpp>

#include <any>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/adk/interrupt.hpp"
#include "wh/compose/interrupt.hpp"
#include "wh/compose/resume.hpp"
#include "wh/core/address.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/types/callback_types.hpp"
#include "wh/internal/callbacks.hpp"
#include "wh/scheduler/context_helper.hpp"
#include "wh/testing/callback_sequence_assert.hpp"
#include "wh/testing/mock/agent_mock.hpp"
#include "wh/testing/mock/chat_model_mock.hpp"
#include "wh/testing/mock/document_mock.hpp"
#include "wh/testing/mock/embedding_mock.hpp"
#include "wh/testing/mock/indexer_mock.hpp"
#include "wh/testing/mock/retriever_mock.hpp"
#include "wh/testing/mock/stream_mock.hpp"
#include "wh/testing/revision_checkpoint_fixture.hpp"
#include "wh/testing/static_thread_scheduler_helper.hpp"

TEST_CASE("callback manager ordering timing and run-context bridge",
          "[core][callback][condition]") {
  auto manager = std::make_shared<wh::internal::callback_manager>();
  wh::core::run_context context{};
  context.callback_manager = manager;

  std::vector<std::string> trace{};

  auto make_handler =
      [&trace](const std::string &name) -> wh::core::callback_stage_handler {
    return [name, &trace](const wh::core::callback_stage stage,
                          const wh::core::callback_event_view event) {
      const auto *payload = event.get_if<int>();
      REQUIRE(payload != nullptr);
      trace.push_back(name + ":" + std::to_string(static_cast<int>(stage)) +
                      ":" + std::to_string(*payload));
    };
  };

  const auto always = wh::internal::make_callback_config(
      [](const wh::core::callback_stage) noexcept { return true; }, "always");
  const auto start_only = wh::internal::make_callback_config(
      [](const wh::core::callback_stage stage) noexcept {
        return stage == wh::core::callback_stage::start;
      },
      "start-only");

  REQUIRE(
      manager->register_global_handler(always, make_handler("g1")).has_value());
  REQUIRE(manager->register_global_handler(start_only, make_handler("g2"))
              .has_value());
  REQUIRE(
      manager->register_local_handler(always, make_handler("l1")).has_value());
  REQUIRE(
      manager->register_local_handler(always, make_handler("l2")).has_value());

  const auto started = wh::internal::inject_callback_event(
      context, wh::core::callback_stage::start, 7);
  REQUIRE(started.has_value());
  REQUIRE(trace ==
          std::vector<std::string>{"l2:0:7", "l1:0:7", "g2:0:7", "g1:0:7"});

  trace.clear();
  const auto ended = wh::internal::inject_callback_event(
      context, wh::core::callback_stage::end, 7);
  REQUIRE(ended.has_value());
  REQUIRE(trace == std::vector<std::string>{"g1:1:7", "l1:1:7", "l2:1:7"});

  wh::core::run_context no_callback_context{};
  const auto no_callback_status = wh::internal::inject_callback_event(
      no_callback_context, wh::core::callback_stage::start, 9);
  REQUIRE(no_callback_status.has_value());
}

TEST_CASE("run context stores reference-based session values",
          "[core][context][condition]") {
  wh::core::run_context context{};
  wh::core::set_session_value(context, "large-payload",
                              std::string{"runtime-value"});

  const auto payload_ref =
      wh::core::session_value_ref<std::string>(context, "large-payload");
  REQUIRE(payload_ref.has_value());
  REQUIRE(payload_ref.value().get() == "runtime-value");

  const auto consumed =
      wh::core::consume_session_value<std::string>(context, "large-payload");
  REQUIRE(consumed.has_value());
  REQUIRE(consumed.value() == "runtime-value");
  REQUIRE_FALSE(
      wh::core::session_value_ref<std::string>(context, "large-payload")
          .has_value());
}

TEST_CASE("fatal exception is converted to callback error event",
          "[core][callback][boundary]") {
  auto manager = std::make_shared<wh::internal::callback_manager>();
  wh::core::run_context context{};
  context.callback_manager = manager;

  std::vector<wh::core::callback_fatal_error> captured_errors{};
  REQUIRE(
      manager
          ->register_global_handler(
              wh::internal::make_callback_config(
                  [](const wh::core::callback_stage stage) noexcept {
                    return stage == wh::core::callback_stage::error;
                  }),
              [&captured_errors](const wh::core::callback_stage,
                                 const wh::core::callback_event_view event) {
                const auto *fatal_error =
                    event.get_if<wh::core::callback_fatal_error>();
                REQUIRE(fatal_error != nullptr);
                captured_errors.push_back(*fatal_error);
              })
          .has_value());

  const auto status = wh::internal::run_with_fatal_event(
      context, [] { throw std::runtime_error{"boom"}; });
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::internal_error);

  REQUIRE(captured_errors.size() == 1U);
  REQUIRE(captured_errors.front().code == wh::core::errc::internal_error);
  REQUIRE(captured_errors.front().exception_message == "boom");
  REQUIRE_FALSE(captured_errors.front().to_string().empty());
}

TEST_CASE("address interrupt and resume paths are stable and consumable",
          "[core][resume][condition]") {
  const wh::core::address root{"graph"};
  const wh::core::address branch = root.append("planner");
  const wh::core::address leaf = branch.append("tool");

  REQUIRE(root.to_string() == "graph");
  REQUIRE(branch.to_string() == "graph/planner");
  REQUIRE(leaf.starts_with(root));
  REQUIRE(root != leaf);

  wh::core::resume_state state{};
  REQUIRE(wh::compose::add_resume_target(state, "interrupt-A", leaf, 3)
              .has_value());
  REQUIRE(wh::compose::add_resume_target(state, "interrupt-B",
                                         branch.append("memory"),
                                         std::string{"recover"})
              .has_value());

  REQUIRE(state.is_resume_target(root));
  REQUIRE_FALSE(state.is_exact_resume_target(branch));
  REQUIRE(state.is_exact_resume_target(leaf));

  const auto next_children = wh::compose::next_resume_points(state, root);
  REQUIRE(next_children == std::vector<std::string>{"planner"});

  const auto recovered_integer =
      wh::compose::consume_resume_data<int>(state, "interrupt-A");
  REQUIRE(recovered_integer.has_value());
  REQUIRE(recovered_integer.value() == 3);

  const auto consumed_again =
      wh::compose::consume_resume_data<int>(state, "interrupt-A");
  REQUIRE(consumed_again.has_error());
  REQUIRE(consumed_again.error() == wh::core::errc::contract_violation);

  const auto wrong_type =
      wh::compose::consume_resume_data<int>(state, "interrupt-B");
  REQUIRE(wrong_type.has_error());
  REQUIRE(wrong_type.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("resume merge accepts rvalue state without data copy path",
          "[core][resume][condition]") {
  wh::core::resume_state base{};
  wh::core::resume_state delta{};

  REQUIRE(wh::compose::add_resume_target(
              delta, "interrupt-move",
              wh::core::address{"root"}.append("moved-node"),
              std::string{"payload"})
              .has_value());

  REQUIRE(wh::compose::merge_resume_state(base, std::move(delta)).has_value());
  REQUIRE(delta.empty());

  const auto moved =
      wh::compose::consume_resume_data<std::string>(base, "interrupt-move");
  REQUIRE(moved.has_value());
  REQUIRE(moved.value() == "payload");
}

TEST_CASE("interrupt conversion keeps id address and state mappings",
          "[core][interrupt][condition]") {
  const std::array<wh::core::interrupt_signal, 2U> signals{
      wh::compose::make_interrupt_signal("id-1", {"root", "child"}, 10),
      wh::compose::make_interrupt_signal("id-2", {"root", "leaf"},
                                         std::string{"state"}),
  };

  const auto snapshot = wh::compose::flatten_interrupt_signals(signals);
  REQUIRE(snapshot.interrupt_id_to_address.size() == 2U);
  REQUIRE(snapshot.interrupt_id_to_state.size() == 2U);

  const auto contexts = wh::adk::to_interrupt_contexts(signals);
  REQUIRE(contexts.size() == 2U);
  REQUIRE(contexts.front().interrupt_id == "id-1");

  const auto rebuilt_signals = wh::adk::from_interrupt_contexts(contexts);
  REQUIRE(rebuilt_signals.size() == 2U);
  REQUIRE(rebuilt_signals.back().interrupt_id == "id-2");
}

TEST_CASE("interrupt id tree bridge and whitelist projection are stable",
          "[core][interrupt][condition]") {
  const auto generated_a =
      wh::compose::make_interrupt_signal(wh::core::address{"root", "alpha"}, 1);
  const auto generated_b =
      wh::compose::make_interrupt_signal(wh::core::address{"root", "beta"}, 2);
  REQUIRE_FALSE(generated_a.interrupt_id.empty());
  REQUIRE(generated_a.interrupt_id != generated_b.interrupt_id);

  const std::array<wh::core::interrupt_signal, 3U> signals{
      wh::compose::make_interrupt_signal("id-1", {"root", "branch", "leaf-a"},
                                         std::string{"state-a"}),
      wh::compose::make_interrupt_signal("id-2", {"root", "branch", "leaf-b"},
                                         std::string{"state-b"}),
      wh::compose::make_interrupt_signal("id-3", {"root", "other", "leaf-c"},
                                         std::string{"state-c"}),
  };

  const auto signal_tree = wh::compose::rebuild_interrupt_signal_tree(signals);
  REQUIRE(signal_tree.size() == 1U);
  REQUIRE(signal_tree.front().location.to_string() == "root");
  REQUIRE(signal_tree.front().children.size() == 2U);

  const auto context_tree = wh::compose::to_interrupt_context_tree(signal_tree);
  const auto roundtrip_tree =
      wh::compose::to_interrupt_signal_tree(context_tree);
  const auto flattened =
      wh::compose::flatten_interrupt_signal_tree(roundtrip_tree);
  REQUIRE(flattened.size() == signals.size());

  const wh::core::interrupt_context context{
      "id-proj", wh::core::address{"root", "private", "visible", "tool"},
      std::string{"state"}, std::any{}, false};
  constexpr std::array<std::string_view, 2U> allowed_segments{"root",
                                                              "visible"};
  const auto projected =
      wh::compose::project_interrupt_context(context, allowed_segments);
  REQUIRE(projected.location.to_string() == "root/visible");
}

TEST_CASE("callback sequence assert helper records stable ordering",
          "[core][callback][condition]") {
  wh::testing::callback_sequence_assert trace{};
  trace.record(wh::core::callback_stage::start);
  trace.record(wh::core::callback_stage::end);
  trace.record(wh::core::callback_stage::error);

  REQUIRE(trace
              .expect({wh::core::callback_stage::start,
                       wh::core::callback_stage::end,
                       wh::core::callback_stage::error})
              .has_value());
  REQUIRE(trace
              .expect({wh::core::callback_stage::start,
                       wh::core::callback_stage::error})
              .has_error());
}

TEST_CASE("static thread scheduler helper provides test scheduler context",
          "[core][testing][condition]") {
  wh::testing::static_thread_scheduler_helper helper{};
  const auto context = helper.context();

  auto sender =
      stdexec::schedule(wh::core::select_execution_scheduler(context)) |
      stdexec::then([] { return 42; });
  auto result = stdexec::sync_wait(std::move(sender));
  REQUIRE(result.has_value());
  REQUIRE(std::get<0>(result.value()) == 42);
}

TEST_CASE("stream mock supports eof policy and scripted errors",
          "[core][mock][condition]") {
  wh::testing::mock::stream_mock stream{};
  stream.enqueue_chunk("chunk-a");
  stream.enqueue_chunk("chunk-b");

  const auto first = stream.next();
  REQUIRE(first.has_value());
  REQUIRE(first.value() == "chunk-a");
  const auto second = stream.next();
  REQUIRE(second.has_value());
  REQUIRE(second.value() == "chunk-b");

  stream.set_eof_behavior(wh::testing::mock::stream_mock::eof_behavior::close);
  const auto closed = stream.next();
  REQUIRE(closed.has_error());
  REQUIRE(closed.error() == wh::core::errc::channel_closed);

  stream.set_eof_behavior(wh::testing::mock::stream_mock::eof_behavior::error,
                          wh::core::errc::internal_error);
  const auto forced_error = stream.next();
  REQUIRE(forced_error.has_error());
  REQUIRE(forced_error.error() == wh::core::errc::internal_error);

  wh::testing::mock::stream_mock repeat_stream{};
  repeat_stream.enqueue_chunk("tail");
  repeat_stream.set_eof_behavior(
      wh::testing::mock::stream_mock::eof_behavior::repeat_last);
  const auto tail = repeat_stream.next();
  REQUIRE(tail.has_value());
  REQUIRE(tail.value() == "tail");
  const auto repeated = repeat_stream.next();
  REQUIRE(repeated.has_value());
  REQUIRE(repeated.value() == "tail");
}

TEST_CASE("revision checkpoint fixture preserves revision and resume snapshot",
          "[core][testing][condition]") {
  wh::core::resume_state state{};
  state.bind_revision(7U);
  REQUIRE(wh::compose::add_resume_target(state, "interrupt-rv",
                                         wh::core::address{"root", "node"},
                                         std::string{"payload"})
              .has_value());

  wh::testing::revision_checkpoint_fixture fixture{};
  const auto snapshot = fixture.capture(state);
  REQUIRE(snapshot.revision == 7U);

  auto restored = fixture.restore(snapshot);
  REQUIRE(fixture.revision() == 7U);
  REQUIRE(restored.revision() == 7U);
  const auto consumed =
      wh::compose::consume_resume_data<std::string>(restored, "interrupt-rv");
  REQUIRE(consumed.has_value());
  REQUIRE(consumed.value() == "payload");
}

TEST_CASE("testing mocks are deterministic and support injections",
          "[core][mock][condition]") {
  wh::testing::mock::agent_mock agent{};
  agent.enqueue_success("agent-ok");
  agent.enqueue_interrupt();
  const auto first_agent = agent.run("input");
  REQUIRE(first_agent.has_value());
  REQUIRE(first_agent.value() == "agent-ok");
  const auto second_agent = agent.run("input");
  REQUIRE(second_agent.has_error());
  REQUIRE(second_agent.error() == wh::core::errc::canceled);

  wh::testing::mock::chat_model_mock chat_model{};
  chat_model.enqueue_success("chat-ok");
  REQUIRE(chat_model.complete("prompt").value() == "chat-ok");

  wh::testing::mock::document_mock document{};
  document.enqueue_success("doc");
  REQUIRE(document.load("doc-id").value() == "doc");

  wh::testing::mock::embedding_mock embedding{};
  embedding.enqueue_success(std::vector<float>{1.0F, 2.0F});
  const auto embedded = embedding.embed("text");
  REQUIRE(embedded.has_value());
  REQUIRE(embedded.value().size() == 2U);

  wh::testing::mock::indexer_mock indexer{};
  indexer.enqueue_success();
  REQUIRE(indexer.upsert("doc-id", {0.1F, 0.2F}).has_value());

  wh::testing::mock::retriever_mock retriever{};
  retriever.enqueue_success({"a", "b"});
  const auto retrieved = retriever.retrieve("query");
  REQUIRE(retrieved.has_value());
  REQUIRE(retrieved.value().size() == 2U);
}
