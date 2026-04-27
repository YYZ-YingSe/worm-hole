#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"
#include "wh/agent/chat.hpp"

namespace {

template <typename shell_t>
concept raw_model_settable = requires(shell_t shell, wh::testing::helper::sync_probe_model model) {
  shell.set_model(std::move(model));
};

template <typename shell_t>
concept binding_settable = requires(shell_t shell, wh::agent::model_binding binding) {
  shell.set_model(std::move(binding));
};

static_assert(!raw_model_settable<wh::agent::chat>);
static_assert(binding_settable<wh::agent::chat>);

} // namespace

TEST_CASE("chat shell binds model instruction output controls and lowers into executable agent",
          "[UT][wh/agent/chat.hpp][chat::freeze][condition][branch][boundary]") {
  REQUIRE(wh::agent::chat_model_node_key == "__chat_model__");

  wh::agent::chat authored{"chat", "assistant"};
  REQUIRE(authored.name() == "chat");
  REQUIRE(authored.description() == "assistant");
  REQUIRE_FALSE(authored.frozen());
  REQUIRE(authored.output_key().empty());
  REQUIRE(authored.output_mode() == wh::agent::chat_output_mode::value);
  REQUIRE(authored.model_binding().has_error());
  REQUIRE(authored.model_binding().error() == wh::core::errc::not_found);

  REQUIRE(authored.append_instruction("system").has_value());
  REQUIRE(authored.replace_instruction("override").has_value());
  REQUIRE(authored.render_instruction(" | ") == "override | system");
  REQUIRE(authored.set_output_key("final").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::chat_output_mode::text).has_value());
  REQUIRE(authored.output_key() == "final");
  REQUIRE(authored.output_mode() == wh::agent::chat_output_mode::text);

  auto model_state = std::make_shared<wh::testing::helper::probe_model_state>();
  REQUIRE(authored
              .set_model(wh::testing::helper::make_sync_probe_model_binding(
                  wh::testing::helper::sync_probe_model{model_state}))
              .has_value());
  REQUIRE(authored.model_binding().has_value());
  REQUIRE(authored.freeze().has_value());
  REQUIRE(authored.frozen());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered.value().name() == "chat");
  REQUIRE(lowered.value().executable());
}

TEST_CASE("chat shell validates required fields and rejects mutation after freeze",
          "[UT][wh/agent/chat.hpp][chat::set_model][condition][branch][boundary]") {
  wh::agent::chat missing_model{"chat", "assistant"};
  auto missing_model_freeze = missing_model.freeze();
  REQUIRE(missing_model_freeze.has_error());
  REQUIRE(missing_model_freeze.error() == wh::core::errc::not_found);

  wh::agent::chat missing_description{"chat", ""};
  REQUIRE(missing_description.set_model(wh::testing::helper::make_sync_probe_model_binding())
              .has_value());
  auto missing_description_freeze = missing_description.freeze();
  REQUIRE(missing_description_freeze.has_error());
  REQUIRE(missing_description_freeze.error() == wh::core::errc::invalid_argument);

  wh::agent::chat frozen = wh::testing::helper::make_configured_chat("frozen-chat", "assistant");
  REQUIRE(frozen.freeze().has_value());
  REQUIRE(frozen.append_instruction("late").has_error());
  REQUIRE(frozen.replace_instruction("late").has_error());
  REQUIRE(frozen.set_output_key("late").has_error());
  REQUIRE(frozen.set_output_mode(wh::agent::chat_output_mode::value).has_error());
  REQUIRE(frozen.set_model(wh::testing::helper::make_sync_probe_model_binding()).has_error());
  REQUIRE(frozen.append_instruction("late").error() == wh::core::errc::contract_violation);
}

TEST_CASE("chat shell accepts async model bindings with the same native boundary",
          "[UT][wh/agent/chat.hpp][chat::set_model][async][boundary]") {
  wh::agent::chat authored{"chat", "assistant"};
  REQUIRE(authored.set_model(wh::testing::helper::make_async_probe_model_binding()).has_value());
  REQUIRE(authored.freeze().has_value());
}

TEST_CASE("chat shell accepts value model bindings without shell-level contract narrowing",
          "[UT][wh/agent/chat.hpp][chat::freeze][value-output][boundary]") {
  wh::agent::chat sync_authored{"chat-sync-value", "assistant"};
  REQUIRE(sync_authored.set_model(wh::testing::helper::make_sync_probe_model_value_binding())
              .has_value());
  REQUIRE(sync_authored.freeze().has_value());

  wh::agent::chat async_authored{"chat-async-value", "assistant"};
  REQUIRE(async_authored.set_model(wh::testing::helper::make_async_probe_model_value_binding())
              .has_value());
  REQUIRE(async_authored.freeze().has_value());
}
