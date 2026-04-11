#include <memory>
#include <optional>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/agent_tool.hpp"
#include "wh/adk/detail/agent_tool_impl.hpp"

namespace {

[[nodiscard]] auto make_call_scope(wh::core::run_context &context) -> wh::tool::call_scope {
  return wh::tool::call_scope{
      .run = context,
      .component = "agent_tool_impl_ut",
      .implementation = "agent_tool_impl_ut",
      .tool_name = "delegate",
      .call_id = "call-1",
  };
}

} // namespace

TEST_CASE("agent tool impl detail constants runtime access and metadata helpers stay stable",
          "[UT][wh/adk/detail/"
          "agent_tool_impl.hpp][agent_tool_access::make_runtime][condition][branch][boundary]") {
  REQUIRE(wh::adk::detail::agent_tool_interrupt_id_prefix == "tool:");
  REQUIRE(wh::adk::detail::agent_tool_interrupt_default_suffix == "interrupt");
  REQUIRE(wh::adk::detail::agent_tool_interrupt_reason == "agent tool interrupted");
  REQUIRE(wh::adk::detail::agent_tool_request_json_key == "request");

  auto message = wh::adk::detail::make_user_message("hello");
  REQUIRE(message.role == wh::schema::message_role::user);
  REQUIRE(wh::adk::detail::render_message_text(message) == "hello");

  wh::adk::agent_tool unbound{"delegate", "delegate request", wh::agent::agent{"worker"}};
  auto missing_runtime = wh::adk::detail::agent_tool_access::make_runtime(unbound);
  REQUIRE(missing_runtime.has_error());
  REQUIRE(missing_runtime.error() == wh::core::errc::not_found);

  wh::adk::agent_tool tool{"delegate", "delegate request", wh::agent::agent{"worker"}};
  REQUIRE(tool.bind_runner([](const wh::adk::run_request &, wh::core::run_context &)
                               -> wh::adk::agent_run_result { return wh::adk::agent_run_output{}; })
              .has_value());
  auto runtime = wh::adk::detail::agent_tool_access::make_runtime(tool);
  REQUIRE(runtime.has_value());
  REQUIRE(runtime->tool_name == "delegate");
  REQUIRE(runtime->agent_name == "worker");
  REQUIRE(runtime->input_mode == wh::adk::agent_tool_input_mode::request);

  wh::core::run_context context{};
  auto scope = make_call_scope(context);
  auto snapshot = wh::adk::detail::make_agent_tool_scope_snapshot(scope);
  REQUIRE(snapshot.call_id == "call-1");
  REQUIRE(snapshot.location.to_string("/") == "tool/delegate/call-1");

  auto default_metadata = wh::adk::detail::default_tool_metadata(runtime.value(), snapshot);
  REQUIRE(default_metadata.agent_name == "worker");
  REQUIRE(default_metadata.tool_name == "delegate");
  REQUIRE(default_metadata.run_path.to_string("/") == "tool/delegate/call-1/agent/worker");

  wh::adk::event_metadata child_metadata{};
  child_metadata.run_path = wh::adk::run_path{{"agent", "leaf"}};
  auto normalized = wh::adk::detail::normalize_child_metadata(runtime.value(), snapshot,
                                                              std::move(child_metadata));
  REQUIRE(normalized.run_path.to_string("/") == "tool/delegate/call-1/agent/leaf");
  REQUIRE(normalized.agent_name == "worker");
  REQUIRE(normalized.tool_name == "delegate");
}

TEST_CASE(
    "agent tool impl detail captures bridge state reifies events and preserves interrupt snapshots",
    "[UT][wh/adk/detail/agent_tool_impl.hpp][make_agent_tool_event][condition][branch][boundary]") {
  wh::core::any move_only_payload{std::make_unique<int>(3)};
  auto move_only_owned = wh::adk::detail::capture_bridge_state_any_or_empty(move_only_payload);
  REQUIRE_FALSE(move_only_owned.has_value());

  std::string borrowed_text = "borrowed";
  auto borrowed_owned =
      wh::adk::detail::capture_bridge_state_any_or_empty(wh::core::any::ref(borrowed_text));
  auto *owned_text = wh::core::any_cast<std::string>(&borrowed_owned);
  REQUIRE(owned_text != nullptr);
  REQUIRE(*owned_text == "borrowed");
  borrowed_text = "mutated";
  REQUIRE(*owned_text == "borrowed");

  wh::adk::event_metadata copied_metadata{};
  copied_metadata.attributes.emplace("copyable", wh::core::any{7});
  copied_metadata.attributes.emplace("move-only", wh::core::any{std::make_unique<int>(9)});
  auto copied_owned_metadata = wh::adk::detail::capture_bridge_metadata(copied_metadata);
  REQUIRE(copied_owned_metadata.attributes.contains("copyable"));
  REQUIRE_FALSE(copied_owned_metadata.attributes.contains("move-only"));

  wh::adk::event_metadata moved_metadata{};
  moved_metadata.attributes.emplace("copyable", wh::core::any{7});
  moved_metadata.attributes.emplace("move-only", wh::core::any{std::make_unique<int>(9)});
  auto moved_owned_metadata = wh::adk::detail::capture_bridge_metadata(std::move(moved_metadata));
  REQUIRE(moved_owned_metadata.attributes.contains("copyable"));
  REQUIRE(moved_owned_metadata.attributes.contains("move-only"));
  auto *move_only_attribute =
      wh::core::any_cast<std::unique_ptr<int>>(&moved_owned_metadata.attributes.at("move-only"));
  REQUIRE(move_only_attribute != nullptr);
  REQUIRE(**move_only_attribute == 9);

  auto message_event =
      wh::adk::detail::make_agent_tool_event(wh::adk::detail::agent_tool_event_record{
          .payload =
              wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "chunk"),
          .metadata = {},
      });
  REQUIRE(std::holds_alternative<wh::adk::message_event>(message_event.payload));

  auto custom_event =
      wh::adk::detail::make_agent_tool_event(wh::adk::detail::agent_tool_event_record{
          .payload =
              wh::adk::detail::agent_tool_custom_event_record{
                  .name = "custom",
                  .payload = wh::core::any{std::string{"payload"}},
              },
          .metadata = {},
      });
  REQUIRE(std::holds_alternative<wh::adk::custom_event>(custom_event.payload));

  auto error_event =
      wh::adk::detail::make_agent_tool_event(wh::adk::detail::agent_tool_event_record{
          .payload =
              wh::adk::detail::agent_tool_error_event_record{
                  .code = wh::core::make_error_code(wh::core::errc::unavailable),
                  .message = "failed",
                  .detail = wh::core::any{std::string{"detail"}},
              },
          .metadata = {},
      });
  REQUIRE(std::holds_alternative<wh::adk::error_event>(error_event.payload));

  wh::adk::detail::agent_tool_output_summary output{};
  output.text_chunks = {"a", "b"};
  output.final_message =
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "done");
  auto checkpoint = wh::adk::detail::make_agent_tool_checkpoint_state(output);
  auto roundtrip = wh::adk::detail::make_agent_tool_output_summary(checkpoint);
  REQUIRE(roundtrip.text_chunks == std::vector<std::string>{"a", "b"});
  REQUIRE(roundtrip.final_message.has_value());

  wh::core::interrupt_context interrupt{};
  interrupt.interrupt_id = "child";
  interrupt.location = wh::core::address{{"agent", "worker"}};
  interrupt.state = wh::core::any{std::string{"state"}};
  interrupt.layer_payload = wh::core::any{std::string{"payload"}};
  interrupt.parent_locations = {wh::core::address{{"graph", "root"}}};
  interrupt.trigger_reason = "child interrupted";
  auto child = wh::adk::detail::make_child_interrupt_record(interrupt);
  REQUIRE(child.interrupt_id == "child");
  auto restored = wh::adk::detail::to_interrupt_context(child);
  REQUIRE(restored.interrupt_id == "child");
  REQUIRE(restored.location.to_string("/") == "agent/worker");

  auto owned_interrupt =
      wh::core::into_owned(wh::core::any{wh::adk::detail::agent_tool_interrupt_state{
          .checkpoint = checkpoint,
          .child_interrupt = child,
      }});
  REQUIRE(owned_interrupt.has_value());
}

TEST_CASE(
    "agent tool impl detail parses requests builds run inputs and materializes terminal values",
    "[UT][wh/adk/detail/"
    "agent_tool_impl.hpp][materialize_agent_tool_value][condition][branch][boundary]") {
  auto parsed = wh::adk::detail::parse_request_text(R"({"request":"hello"})");
  REQUIRE(parsed.has_value());
  REQUIRE(*parsed == "hello");
  auto invalid_json = wh::adk::detail::parse_request_text("[]");
  REQUIRE(invalid_json.has_error());

  wh::adk::detail::agent_tool_runtime request_runtime{
      .tool_name = "delegate",
      .agent_name = "worker",
      .input_mode = wh::adk::agent_tool_input_mode::request,
  };
  wh::compose::tool_call request_call{
      .call_id = "call-1",
      .tool_name = "delegate",
      .arguments = R"({"request":"hello bridge"})",
  };
  wh::core::run_context context{};
  auto request = wh::adk::detail::build_agent_tool_request(request_runtime, request_call, context);
  REQUIRE(request.has_value());
  REQUIRE(request->messages.size() == 1U);

  wh::adk::detail::agent_tool_runtime custom_runtime{
      .tool_name = "delegate",
      .agent_name = "worker",
      .input_mode = wh::adk::agent_tool_input_mode::custom_schema,
  };
  auto custom_request =
      wh::adk::detail::build_agent_tool_request(custom_runtime, request_call, context);
  REQUIRE(custom_request.has_value());
  REQUIRE(custom_request->messages.size() == 1U);

  wh::model::chat_request history_request{};
  history_request.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "history"));
  wh::adk::detail::history_request_payload history_payload{
      .history_request = history_request,
  };
  wh::compose::tool_call history_call{
      .call_id = "call-2",
      .tool_name = "delegate",
      .payload = wh::core::any{history_payload},
  };
  wh::adk::detail::agent_tool_runtime history_runtime{
      .tool_name = "delegate",
      .agent_name = "worker",
      .input_mode = wh::adk::agent_tool_input_mode::message_history,
  };
  auto history = wh::adk::detail::build_agent_tool_request(history_runtime, history_call, context);
  REQUIRE(history.has_value());
  REQUIRE(history->messages.size() == 1U);

  REQUIRE(wh::adk::detail::default_message_history_schema().find("messages") != std::string::npos);

  auto value = wh::adk::detail::materialize_agent_tool_value(wh::adk::agent_tool_result{
      .output_text = "joined",
  });
  REQUIRE(value.has_value());
  auto *joined = wh::core::any_cast<std::string>(&value.value());
  REQUIRE(joined != nullptr);
  REQUIRE(*joined == "joined");

  auto interrupted = wh::adk::detail::materialize_agent_tool_value(
      wh::adk::agent_tool_result{.interrupted = true});
  REQUIRE(interrupted.has_error());
  REQUIRE(interrupted.error() == wh::core::errc::canceled);

  auto failed = wh::adk::detail::materialize_agent_tool_value(wh::adk::agent_tool_result{
      .final_error = wh::core::make_error_code(wh::core::errc::unavailable),
  });
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::unavailable);

  auto empty = wh::adk::detail::materialize_agent_tool_value(wh::adk::agent_tool_result{});
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::errc::protocol_error);
}
