#include <string>

#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/react.hpp"

namespace {

template <typename shell_t>
concept raw_model_settable = requires(shell_t shell, wh::testing::helper::sync_probe_model model) {
  shell.set_model(std::move(model));
};

template <typename shell_t>
concept binding_settable = requires(shell_t shell, wh::agent::model_binding binding) {
  shell.set_model(std::move(binding));
};

static_assert(!raw_model_settable<wh::agent::react>);
static_assert(binding_settable<wh::agent::react>);

} // namespace

TEST_CASE("react shell records tool authoring model policy and lowers into executable agent",
          "[UT][wh/agent/react.hpp][react::freeze][condition][branch][boundary]") {
  REQUIRE(wh::agent::react_model_node_key == "__react_model__");

  wh::agent::react authored{"react", "assistant"};
  REQUIRE(authored.name() == "react");
  REQUIRE(authored.description() == "assistant");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.max_iterations() == 20U);
  REQUIRE(authored.output_key().empty());
  REQUIRE(authored.output_mode() == wh::agent::react_output_mode::value);
  REQUIRE(authored.model_binding().has_error());

  REQUIRE(authored.append_instruction("system").has_value());
  REQUIRE(authored.replace_instruction("override").has_value());
  REQUIRE(authored.render_instruction(" | ") == "override | system");
  REQUIRE(authored.set_max_iterations(0U).has_value());
  REQUIRE(authored.max_iterations() == 1U);
  REQUIRE(authored.set_output_key("final").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::react_output_mode::stream).has_value());

  auto schema = wh::schema::tool_schema_definition{
      .name = "manual",
      .description = "manual tool",
  };
  wh::compose::tool_entry entry{};
  entry.invoke = [](const wh::compose::tool_call &call,
                    const wh::tool::call_scope &) -> wh::core::result<wh::compose::graph_value> {
    return wh::compose::graph_value{call.arguments};
  };
  REQUIRE(authored.add_tool_entry(schema, entry).has_value());
  REQUIRE(authored.add_tool(wh::testing::helper::sync_tool{}).has_value());
  REQUIRE(authored.add_tool(wh::testing::helper::async_stream_tool{}).has_value());
  REQUIRE(authored.add_tool_middleware({}).has_value());
  REQUIRE(authored
              .set_tools_node_options(
                  {.exec_mode = wh::compose::node_exec_mode::async, .sequential = false})
              .has_value());
  REQUIRE(authored.tools().size() == 3U);
  REQUIRE(authored.tools().runtime_options().middleware.size() == 1U);
  REQUIRE(authored.tools().node_options().has_value());

  auto state = std::make_shared<wh::testing::helper::probe_model_state>();
  REQUIRE(authored
              .set_model(wh::testing::helper::make_sync_probe_model_binding(
                  wh::testing::helper::sync_probe_model{state}))
              .has_value());
  REQUIRE(authored.model_binding().has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().name() == "react");
  REQUIRE(lowered.value().executable());
}

TEST_CASE("react history rewriting turns assistant tool history into neutral context prompts",
          "[UT][wh/agent/"
          "react.hpp][rewrite_history_message_as_context_prompt][condition][branch][boundary]") {
  wh::schema::message assistant{};
  assistant.role = wh::schema::message_role::assistant;
  assistant.parts.emplace_back(wh::schema::text_part{"draft"});
  assistant.parts.emplace_back(wh::schema::tool_call_part{
      .index = 0U,
      .id = "call-1",
      .type = "function",
      .name = "search",
      .arguments = "{\"q\":\"hi\"}",
      .complete = true,
  });
  auto rewritten_assistant = wh::agent::rewrite_history_message_as_context_prompt(assistant);
  REQUIRE(rewritten_assistant.has_value());
  REQUIRE(rewritten_assistant->role == wh::schema::message_role::user);
  const auto &assistant_text =
      std::get<wh::schema::text_part>(rewritten_assistant->parts.front()).text;
  REQUIRE(assistant_text.find("assistant said: draft.") != std::string::npos);
  REQUIRE(assistant_text.find("assistant called tool `search`") != std::string::npos);

  wh::schema::message tool{};
  tool.role = wh::schema::message_role::tool;
  tool.tool_name = "search";
  tool.parts.emplace_back(wh::schema::text_part{"result"});
  auto rewritten_tool = wh::agent::rewrite_history_message_as_context_prompt(tool);
  REQUIRE(rewritten_tool.has_value());
  REQUIRE(std::get<wh::schema::text_part>(rewritten_tool->parts.front())
              .text.find("tool `search` returned result: result.") != std::string::npos);

  auto passthrough = wh::testing::helper::make_text_message(wh::schema::message_role::user, "keep");
  auto rewritten_user = wh::agent::rewrite_history_message_as_context_prompt(passthrough);
  REQUIRE(rewritten_user.has_value());
  REQUIRE(rewritten_user->role == wh::schema::message_role::user);
  REQUIRE(std::get<wh::schema::text_part>(rewritten_user->parts.front()).text == "keep");

  wh::schema::message empty_assistant{};
  empty_assistant.role = wh::schema::message_role::assistant;
  REQUIRE_FALSE(wh::agent::rewrite_history_message_as_context_prompt(empty_assistant).has_value());
}

TEST_CASE("react shell enforces model and tools node options and blocks late mutation",
          "[UT][wh/agent/react.hpp][react::set_tools_node_options][condition][branch][boundary]") {
  wh::agent::react missing_model{"react", "assistant"};
  REQUIRE(missing_model.set_tools_node_options({}).has_value());
  auto missing_model_freeze = missing_model.freeze();
  REQUIRE(missing_model_freeze.has_error());
  REQUIRE(missing_model_freeze.error() == wh::core::errc::not_found);

  wh::agent::react missing_node_options{"react", "assistant"};
  REQUIRE(missing_node_options.set_model(wh::testing::helper::make_sync_probe_model_binding())
              .has_value());
  auto missing_node_options_freeze = missing_node_options.freeze();
  REQUIRE(missing_node_options_freeze.has_error());
  REQUIRE(missing_node_options_freeze.error() == wh::core::errc::contract_violation);

  wh::agent::react frozen = wh::testing::helper::make_configured_react("frozen-react", "assistant");
  REQUIRE(frozen.freeze().has_value());
  REQUIRE(frozen.add_tool_entry({}, {}).has_error());
  REQUIRE(frozen.add_tool(wh::testing::helper::sync_tool{}).has_error());
  REQUIRE(frozen.add_tool_middleware({}).has_error());
  REQUIRE(frozen.set_tools_node_options({}).has_error());
  REQUIRE(frozen.set_max_iterations(2U).has_error());
  REQUIRE(frozen.set_output_key("late").has_error());
  REQUIRE(frozen.set_output_mode(wh::agent::react_output_mode::value).has_error());
  REQUIRE(frozen.set_model(wh::testing::helper::make_sync_probe_model_binding()).has_error());
}

TEST_CASE("react shell accepts async model bindings with the same native boundary",
          "[UT][wh/agent/react.hpp][react::set_model][async][boundary]") {
  wh::agent::react authored{"react", "assistant"};
  REQUIRE(authored.set_tools_node_options({}).has_value());
  REQUIRE(authored.set_model(wh::testing::helper::make_async_probe_model_binding()).has_value());
  REQUIRE(authored.freeze().has_value());
}

TEST_CASE("react shell accepts value model bindings without shell-level contract narrowing",
          "[UT][wh/agent/react.hpp][react::freeze][value-output][boundary]") {
  wh::agent::react sync_authored{"react-sync-value", "assistant"};
  REQUIRE(sync_authored.set_tools_node_options({}).has_value());
  REQUIRE(sync_authored.set_model(wh::testing::helper::make_sync_probe_model_value_binding())
              .has_value());
  REQUIRE(sync_authored.freeze().has_value());

  wh::agent::react async_authored{"react-async-value", "assistant"};
  REQUIRE(async_authored.set_tools_node_options({}).has_value());
  REQUIRE(async_authored.set_model(wh::testing::helper::make_async_probe_model_value_binding())
              .has_value());
  REQUIRE(async_authored.freeze().has_value());
}

TEST_CASE("react shell mounts middleware surfaces and validates request transforms",
          "[UT][wh/agent/react.hpp][react::add_middleware_surface][condition][branch][boundary]") {
  wh::agent::react authored{"react", "assistant"};
  wh::agent::middlewares::middleware_surface surface{};
  surface.instruction_fragments.push_back("filesystem instruction");
  surface.tool_bindings.push_back(
      wh::agent::make_tool_binding_pair(wh::testing::helper::sync_tool{}));
  surface.request_transforms.push_back(wh::agent::middlewares::request_transform_binding{
      .sync = [](wh::model::chat_request request, wh::core::run_context &)
          -> wh::agent::middlewares::request_transform_result { return request; }});
  REQUIRE(authored.add_middleware_surface(std::move(surface)).has_value());
  REQUIRE(authored.render_instruction().find("filesystem instruction") != std::string::npos);
  REQUIRE(authored.tools().size() == 1U);
  REQUIRE(authored.request_transforms().size() == 1U);

  REQUIRE(authored.add_request_transform({}).has_error());
  REQUIRE(authored.add_request_transform({}).error() == wh::core::errc::invalid_argument);
}
