#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/adk/deterministic_transfer.hpp"

namespace {

[[nodiscard]] auto make_named_text_message(const wh::schema::message_role role, std::string name,
                                           std::string text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = role;
  message.name = std::move(name);
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

[[nodiscard]] auto make_message_event(const wh::core::address &path, wh::schema::message message)
    -> wh::adk::agent_event {
  return wh::adk::make_message_event(
      std::move(message), wh::adk::event_metadata{.path = path, .agent_name = "planner"});
}

} // namespace

TEST_CASE("deterministic transfer resolves authored child and parent targets with loop guard",
          "[core][agent][condition]") {
  wh::agent::agent root{"root"};
  wh::agent::agent planner{"planner"};
  REQUIRE(planner.allow_transfer_to_parent().has_value());
  REQUIRE(root.add_child(std::move(planner)).has_value());
  REQUIRE(root.allow_transfer_to_child("planner").has_value());
  REQUIRE(root.freeze().has_value());

  wh::adk::deterministic_transfer_state root_state{};
  root_state.visited_agents.insert("root");
  auto child =
      wh::adk::begin_deterministic_transfer(root, root_state,
                                            wh::adk::transfer_target{
                                                .kind = wh::adk::transfer_target_kind::child,
                                                .agent_name = "planner",
                                            });
  REQUIRE(child.has_value());
  REQUIRE(child.value() == "planner");
  REQUIRE(root_state.pending_target == std::optional<std::string>{"planner"});

  auto repeated =
      wh::adk::begin_deterministic_transfer(root, root_state,
                                            wh::adk::transfer_target{
                                                .kind = wh::adk::transfer_target_kind::child,
                                                .agent_name = "planner",
                                            });
  REQUIRE(repeated.has_error());
  REQUIRE(repeated.error() == wh::core::errc::already_exists);

  auto planner_ref = root.child("planner");
  REQUIRE(planner_ref.has_value());
  wh::adk::deterministic_transfer_state planner_state{};
  planner_state.visited_agents.insert("planner");
  auto parent =
      wh::adk::begin_deterministic_transfer(planner_ref.value().get(), planner_state,
                                            wh::adk::transfer_target{
                                                .kind = wh::adk::transfer_target_kind::parent,
                                            });
  REQUIRE(parent.has_value());
  REQUIRE(parent.value() == "root");
}

TEST_CASE("deterministic transfer resolves raw target names against authored topology",
          "[core][agent][condition]") {
  wh::agent::agent root{"root"};
  wh::agent::agent planner{"planner"};
  REQUIRE(planner.allow_transfer_to_parent().has_value());
  REQUIRE(root.add_child(std::move(planner)).has_value());
  REQUIRE(root.allow_transfer_to_child("planner").has_value());
  REQUIRE(root.freeze().has_value());

  auto to_child = wh::adk::resolve_transfer_target(root, "planner");
  REQUIRE(to_child.has_value());
  REQUIRE(to_child.value().kind == wh::adk::transfer_target_kind::child);
  REQUIRE(to_child.value().agent_name == "planner");

  auto to_self = wh::adk::resolve_transfer_target(root, "root");
  REQUIRE(to_self.has_value());
  REQUIRE(to_self.value().kind == wh::adk::transfer_target_kind::current);

  auto planner_ref = root.child("planner");
  REQUIRE(planner_ref.has_value());
  auto to_parent = wh::adk::resolve_transfer_target(planner_ref.value().get(), "root");
  REQUIRE(to_parent.has_value());
  REQUIRE(to_parent.value().kind == wh::adk::transfer_target_kind::parent);

  auto missing = wh::adk::resolve_transfer_target(root, "ghost");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("deterministic transfer enforces whitelist before resolving targets",
          "[core][agent][boundary]") {
  wh::agent::agent root{"root"};
  REQUIRE(root.add_child(wh::agent::agent{"planner"}).has_value());
  REQUIRE(root.freeze().has_value());

  wh::adk::deterministic_transfer_state state{};
  auto blocked =
      wh::adk::begin_deterministic_transfer(root, state,
                                            wh::adk::transfer_target{
                                                .kind = wh::adk::transfer_target_kind::child,
                                                .agent_name = "planner",
                                            });
  REQUIRE(blocked.has_error());
  REQUIRE(blocked.error() == wh::core::errc::contract_violation);
}

TEST_CASE("deterministic transfer records only exact-path non-interrupt message events",
          "[core][agent][condition]") {
  wh::adk::deterministic_transfer_state state{};
  state.exact_run_path = wh::core::address{{"root", "planner"}};

  auto first = wh::adk::record_parent_visible_event(
      state, make_message_event(wh::core::address{{"root", "planner"}},
                                make_named_text_message(wh::schema::message_role::assistant,
                                                        "planner", "visible")));
  REQUIRE(first.has_value());
  REQUIRE(state.visible_history.size() == 1U);

  auto second = wh::adk::record_parent_visible_event(
      state, make_message_event(wh::core::address{{"root", "worker"}},
                                make_named_text_message(wh::schema::message_role::assistant,
                                                        "planner", "hidden")));
  REQUIRE(second.has_value());
  REQUIRE(state.visible_history.size() == 1U);

  auto interrupt = wh::adk::record_parent_visible_event(
      state, wh::adk::make_control_event(
                 wh::adk::control_action{
                     .kind = wh::adk::control_action_kind::interrupt,
                 },
                 wh::adk::event_metadata{
                     .path = wh::core::address{{"root", "planner"}},
                     .agent_name = "planner",
                 }));
  REQUIRE(interrupt.has_value());
  REQUIRE(state.visible_history.size() == 1U);
}

TEST_CASE("deterministic transfer rewrites foreign history and trims transfer pairs",
          "[core][agent][condition]") {
  std::vector<wh::schema::message> history{};
  history.push_back(wh::adk::make_transfer_assistant_message("planner", "call-1"));
  history.push_back(wh::adk::make_transfer_tool_message("planner", "call-1"));
  history.push_back(
      make_named_text_message(wh::schema::message_role::assistant, "planner", "draft"));
  history.push_back(
      make_named_text_message(wh::schema::message_role::tool, "planner", "tool output"));

  auto rewritten = wh::adk::rewrite_transfer_history(history, "worker",
                                                     wh::adk::resolved_transfer_trim_options{
                                                         .trim_assistant_transfer_message = true,
                                                         .trim_tool_transfer_pair = true,
                                                     });
  REQUIRE(rewritten.size() == 2U);
  REQUIRE(rewritten.front().role == wh::schema::message_role::user);
  REQUIRE(rewritten.back().role == wh::schema::message_role::user);
}

TEST_CASE(
    "deterministic transfer appends transfer messages exactly once and skips non-normal completion",
    "[core][agent][condition]") {
  std::vector<wh::schema::message> history{};
  wh::adk::deterministic_transfer_state state{};
  state.pending_target = std::string{"planner"};

  REQUIRE(wh::adk::append_transfer_messages_once(history, state, "planner", "call-1",
                                                 wh::adk::transfer_completion_kind::normal)
              .has_value());
  REQUIRE(history.size() == 2U);
  REQUIRE(state.pending_target == std::nullopt);

  REQUIRE(wh::adk::append_transfer_messages_once(history, state, "planner", "call-1",
                                                 wh::adk::transfer_completion_kind::normal)
              .has_value());
  REQUIRE(history.size() == 2U);

  REQUIRE(wh::adk::append_transfer_messages_once(history, state, "planner", "call-2",
                                                 wh::adk::transfer_completion_kind::exit)
              .has_value());
  REQUIRE(history.size() == 2U);

  const auto &assistant = history.front();
  REQUIRE(assistant.role == wh::schema::message_role::assistant);
  const auto *tool_call = std::get_if<wh::schema::tool_call_part>(&assistant.parts.front());
  REQUIRE(tool_call != nullptr);
  REQUIRE(tool_call->id == "call-1");
  REQUIRE(tool_call->name == wh::adk::deterministic_transfer_tool_name);

  const auto &tool = history.back();
  REQUIRE(tool.role == wh::schema::message_role::tool);
  REQUIRE(tool.tool_call_id == "call-1");
  REQUIRE(tool.tool_name == wh::adk::deterministic_transfer_tool_name);
}

TEST_CASE("deterministic transfer parses normalized transfer tool messages",
          "[core][agent][condition]") {
  auto message = wh::adk::make_transfer_tool_message("planner", "call-9");
  auto parsed = wh::adk::parse_transfer_tool_message(message);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().target_agent_name == "planner");
  REQUIRE(parsed.value().tool_call_id == "call-9");

  auto extracted = wh::adk::extract_transfer_from_message(message);
  REQUIRE(extracted.has_value());
  REQUIRE(extracted->target_agent_name == "planner");
  REQUIRE(extracted->tool_call_id == "call-9");

  auto plain = make_named_text_message(wh::schema::message_role::assistant, "planner", "hello");
  REQUIRE_FALSE(wh::adk::extract_transfer_from_message(plain).has_value());
}
