#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/deterministic_transfer.hpp"

namespace {

[[nodiscard]] auto make_named_text_message(const wh::schema::message_role role,
                                           std::string name, std::string text)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = role;
  message.name = std::move(name);
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

[[nodiscard]] auto make_message_event(const wh::core::address &path,
                                      wh::schema::message message)
    -> wh::adk::agent_event {
  return wh::adk::make_message_event(
      std::move(message),
      wh::adk::event_metadata{.path = path, .agent_name = "planner"});
}

} // namespace

TEST_CASE("deterministic transfer detail helpers classify and normalize transfer message forms",
          "[UT][wh/adk/deterministic_transfer.hpp][make_transfer_assistant_message][condition][branch][boundary]") {
  REQUIRE(wh::adk::deterministic_transfer_tool_name == "transfer_to_agent");

  const auto context = wh::adk::detail::transfer::make_context_message("ctx");
  REQUIRE(context.role == wh::schema::message_role::user);
  REQUIRE(std::get<wh::schema::text_part>(context.parts.front()).text == "ctx");

  wh::schema::message rendered{};
  rendered.role = wh::schema::message_role::assistant;
  rendered.parts.emplace_back(wh::schema::text_part{"draft"});
  rendered.parts.emplace_back(wh::schema::tool_call_part{
      .index = 0U,
      .id = "call-1",
      .type = "function",
      .name = "search",
      .arguments = "{\"q\":\"hi\"}",
      .complete = true,
  });
  REQUIRE(wh::adk::detail::transfer::render_message_text(rendered) ==
          "draft [tool:search]");
  rendered.name = "planner";
  REQUIRE(wh::adk::detail::transfer::make_context_text(rendered).find(
              "[context agent=planner] draft [tool:search]") == 0U);

  auto assistant = wh::adk::make_transfer_assistant_message("planner", "call-1");
  REQUIRE(assistant.role == wh::schema::message_role::assistant);
  REQUIRE(wh::adk::detail::transfer::is_transfer_assistant_message(assistant));
  REQUIRE(wh::adk::detail::transfer::transfer_assistant_call_id(assistant) ==
          std::optional<std::string_view>{"call-1"});

  auto tool = wh::adk::make_transfer_tool_message("planner", "call-1");
  REQUIRE(tool.role == wh::schema::message_role::tool);
  REQUIRE(wh::adk::detail::transfer::is_transfer_tool_message(tool));

  auto parsed = wh::adk::parse_transfer_tool_message(tool);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().target_agent_name == "planner");
  REQUIRE(parsed.value().tool_call_id == "call-1");
  REQUIRE(wh::adk::extract_transfer_from_message(tool).has_value());

  auto plain = make_named_text_message(wh::schema::message_role::assistant,
                                       "planner", "hello");
  REQUIRE_FALSE(wh::adk::detail::transfer::is_transfer_assistant_message(plain));
  REQUIRE_FALSE(wh::adk::detail::transfer::transfer_assistant_call_id(plain)
                    .has_value());
  REQUIRE_FALSE(wh::adk::detail::transfer::is_transfer_tool_message(plain));
  REQUIRE_FALSE(wh::adk::extract_transfer_from_message(plain).has_value());
  REQUIRE(wh::adk::parse_transfer_tool_message(plain).has_error());
  REQUIRE(wh::adk::parse_transfer_tool_message(plain).error() ==
          wh::core::errc::type_mismatch);
}

TEST_CASE("deterministic transfer resolves authored targets records visible history and rewrites foreign messages",
          "[UT][wh/adk/deterministic_transfer.hpp][begin_deterministic_transfer][condition][branch][boundary]") {
  wh::agent::agent root{"root"};
  wh::agent::agent planner{"planner"};
  REQUIRE(planner.allow_transfer_to_parent().has_value());
  REQUIRE(root.add_child(std::move(planner)).has_value());
  REQUIRE(root.allow_transfer_to_child("planner").has_value());
  REQUIRE(root.freeze().has_value());

  auto to_child = wh::adk::resolve_transfer_target(root, "planner");
  REQUIRE(to_child.has_value());
  REQUIRE(to_child.value().kind == wh::adk::transfer_target_kind::child);
  auto to_self = wh::adk::resolve_transfer_target(root, "root");
  REQUIRE(to_self.has_value());
  REQUIRE(to_self.value().kind == wh::adk::transfer_target_kind::current);
  auto missing = wh::adk::resolve_transfer_target(root, "ghost");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  wh::adk::deterministic_transfer_state state{};
  auto empty_target = wh::adk::begin_deterministic_transfer(state, "");
  REQUIRE(empty_target.has_error());
  REQUIRE(empty_target.error() == wh::core::errc::invalid_argument);

  state.visited_agents.insert("root");
  auto started = wh::adk::begin_deterministic_transfer(root, state, to_child.value());
  REQUIRE(started.has_value());
  REQUIRE(started.value() == "planner");
  REQUIRE(state.pending_target == std::optional<std::string>{"planner"});
  auto duplicate = wh::adk::begin_deterministic_transfer(state, "planner");
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);

  wh::adk::deterministic_transfer_state visible{};
  visible.exact_run_path = wh::core::address{{"root", "planner"}};
  REQUIRE(wh::adk::record_parent_visible_event(
              visible,
              make_message_event(wh::core::address{{"root", "planner"}},
                                 make_named_text_message(
                                     wh::schema::message_role::assistant,
                                     "planner", "visible")))
              .has_value());
  REQUIRE(visible.visible_history.size() == 1U);
  REQUIRE(wh::adk::record_parent_visible_event(
              visible,
              make_message_event(wh::core::address{{"root", "worker"}},
                                 make_named_text_message(
                                     wh::schema::message_role::assistant,
                                     "planner", "hidden")))
              .has_value());
  REQUIRE(visible.visible_history.size() == 1U);
  REQUIRE(wh::adk::record_parent_visible_event(
              visible,
              wh::adk::make_control_event(
                  wh::adk::control_action{
                      .kind = wh::adk::control_action_kind::interrupt,
                  },
                  wh::adk::event_metadata{
                      .path = wh::core::address{{"root", "planner"}},
                      .agent_name = "planner",
                  }))
              .has_value());
  REQUIRE(visible.visible_history.size() == 1U);

  wh::adk::agent_message_stream_reader stream_reader{
      wh::schema::stream::make_values_stream_reader(
          std::vector<wh::schema::message>{
              make_named_text_message(wh::schema::message_role::assistant,
                                      "planner", "stream")})};
  auto unsupported_record = wh::adk::record_parent_visible_event(
      visible,
      wh::adk::make_message_event(std::move(stream_reader),
                                  wh::adk::event_metadata{
                                      .path = wh::core::address{
                                          {"root", "planner"}},
                                  }));
  REQUIRE(unsupported_record.has_error());
  REQUIRE(unsupported_record.error() == wh::core::errc::not_supported);

  std::vector<wh::schema::message> history{};
  history.push_back(
      wh::adk::make_transfer_assistant_message("planner", "call-1"));
  history.push_back(wh::adk::make_transfer_tool_message("planner", "call-1"));
  history.push_back(make_named_text_message(wh::schema::message_role::assistant,
                                            "planner", "draft"));
  history.push_back(make_named_text_message(wh::schema::message_role::tool,
                                            "planner", "tool output"));
  auto rewritten = wh::adk::rewrite_transfer_history(
      history, "worker",
      wh::adk::resolved_transfer_trim_options{
          .trim_assistant_transfer_message = true,
          .trim_tool_transfer_pair = true,
      });
  REQUIRE(rewritten.size() == 2U);
  REQUIRE(rewritten.front().role == wh::schema::message_role::user);
  REQUIRE(rewritten.back().role == wh::schema::message_role::user);
}

TEST_CASE("deterministic transfer appends transfer pairs exactly once and skips non-normal completion",
          "[UT][wh/adk/deterministic_transfer.hpp][append_transfer_messages_once][condition][branch][boundary]") {
  std::vector<wh::schema::message> history{};
  wh::adk::deterministic_transfer_state state{};
  state.pending_target = std::string{"planner"};

  auto invalid = wh::adk::append_transfer_messages_once(
      history, state, "", "call-1",
      wh::adk::transfer_completion_kind::normal);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  REQUIRE(wh::adk::append_transfer_messages_once(
              history, state, "planner", "call-1",
              wh::adk::transfer_completion_kind::normal)
              .has_value());
  REQUIRE(history.size() == 2U);
  REQUIRE(state.pending_target == std::nullopt);

  REQUIRE(wh::adk::append_transfer_messages_once(
              history, state, "planner", "call-1",
              wh::adk::transfer_completion_kind::normal)
              .has_value());
  REQUIRE(history.size() == 2U);

  REQUIRE(wh::adk::append_transfer_messages_once(
              history, state, "planner", "call-2",
              wh::adk::transfer_completion_kind::exit)
              .has_value());
  REQUIRE(wh::adk::append_transfer_messages_once(
              history, state, "planner", "call-3",
              wh::adk::transfer_completion_kind::interrupt)
              .has_value());
  REQUIRE(history.size() == 2U);
}
