#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/bind.hpp"

namespace {

using wh::testing::helper::invoke_agent_graph;
using wh::testing::helper::make_text_message;
using wh::testing::helper::message_text;

struct scripted_react_model_state {
  std::size_t bind_calls{0U};
  std::size_t stream_calls{0U};
  std::size_t bound_tool_count{0U};
};

class scripted_react_model {
public:
  explicit scripted_react_model(std::shared_ptr<scripted_react_model_state> state =
                                    std::make_shared<scripted_react_model_state>())
      : state_(std::move(state)) {}

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"ScriptedReactModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &request,
                            wh::core::run_context &context) const -> wh::model::chat_invoke_result {
    static_cast<void>(request);
    static_cast<void>(context);
    return wh::model::chat_response{
        .message = make_text_message(wh::schema::message_role::assistant, "tool loop done")};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &, wh::core::run_context &) const
      -> wh::model::chat_message_stream_result {
    state_->stream_calls += 1U;

    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    if (state_->stream_calls == 1U) {
      message.parts.emplace_back(wh::schema::text_part{"need tool"});
      message.parts.emplace_back(wh::schema::tool_call_part{
          .index = 0U,
          .id = "call-lookup-1",
          .type = "function",
          .name = "lookup",
          .arguments = "{\"query\":\"hello\"}",
          .complete = true,
      });
    } else {
      message.parts.emplace_back(wh::schema::text_part{"tool loop done"});
    }

    return wh::model::chat_message_stream_reader{wh::schema::stream::make_values_stream_reader(
        std::vector<wh::schema::message>{std::move(message)})};
  }

  [[nodiscard]] auto bind_tools(std::span<const wh::schema::tool_schema_definition> tools) const
      -> scripted_react_model {
    state_->bind_calls += 1U;
    state_->bound_tool_count = tools.size();
    return scripted_react_model{state_};
  }

  [[nodiscard]] auto options() const noexcept -> const wh::model::chat_model_options & {
    return options_;
  }

private:
  std::shared_ptr<scripted_react_model_state> state_{};
  wh::model::chat_model_options options_{};
};

} // namespace

TEST_CASE("chat shell public binding lowers and executes final output",
          "[core][agent][chat][functional]") {
  auto authored = wh::testing::helper::make_configured_chat("chat", "assistant");
  REQUIRE(authored.append_instruction("follow policy").has_value());
  REQUIRE(authored.set_output_key("reply").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::chat_output_mode::text).has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->executable());

  auto graph = lowered->lower();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(graph.value(), {wh::testing::helper::make_text_message(
                                                      wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(output->history_messages.size() == 1U);
  auto reply_iter = output->output_values.find("reply");
  REQUIRE(reply_iter != output->output_values.end());
  auto *reply = wh::core::any_cast<std::string>(&reply_iter->second);
  REQUIRE(reply != nullptr);
  REQUIRE(*reply == "ok");
}

TEST_CASE("react shell public binding lowers and executes tool-capable final output",
          "[core][agent][react][functional]") {
  auto authored = wh::testing::helper::make_configured_react("react", "assistant");
  REQUIRE(authored.set_output_key("final").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::react_output_mode::stream).has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->executable());

  auto graph = lowered->lower();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(graph.value(), {wh::testing::helper::make_text_message(
                                                      wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(output->history_messages.size() == 2U);
  auto final_iter = output->output_values.find("final");
  REQUIRE(final_iter != output->output_values.end());
  auto *final_text = wh::core::any_cast<std::string>(&final_iter->second);
  REQUIRE(final_text != nullptr);
  REQUIRE(*final_text == "ok");
}

TEST_CASE("react shell public binding executes real tool-call loop before final output",
          "[core][agent][react][functional]") {
  auto state = std::make_shared<scripted_react_model_state>();

  wh::agent::react authored{"react-loop", "assistant"};
  REQUIRE(authored
              .set_model(wh::agent::make_model_binding<wh::compose::node_contract::value,
                                                       wh::compose::node_contract::stream>(
                  scripted_react_model{state}))
              .has_value());
  REQUIRE(authored.set_tools_node_options(wh::agent::tools_node_authoring_options{}).has_value());
  REQUIRE(authored.set_output_key("final").has_value());
  REQUIRE(authored.set_output_mode(wh::agent::react_output_mode::stream).has_value());

  wh::schema::tool_schema_definition schema{
      .name = "lookup",
      .description = "lookup tool",
  };
  wh::compose::tool_entry entry{};
  entry.invoke = [](const wh::compose::tool_call &call,
                    const wh::tool::call_scope &) -> wh::core::result<wh::compose::graph_value> {
    return wh::compose::graph_value{std::string{"tool:"} + call.arguments};
  };
  REQUIRE(authored.add_tool_entry(std::move(schema), std::move(entry)).has_value());
  REQUIRE(authored.freeze().has_value());

  auto lowered = std::move(authored).into_agent();
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->executable());

  auto graph = lowered->lower();
  REQUIRE(graph.has_value());
  if (!graph->compiled()) {
    REQUIRE(graph->compile().has_value());
  }

  auto output = invoke_agent_graph(graph.value(),
                                   {make_text_message(wh::schema::message_role::user, "hello")});
  REQUIRE(output.has_value());
  REQUIRE(output->transfer == std::nullopt);
  REQUIRE(output->final_message.role == wh::schema::message_role::assistant);
  REQUIRE(message_text(output->final_message) == "tool loop done");
  REQUIRE(output->history_messages.size() >= 3U);

  bool saw_tool_message = false;
  for (const auto &message : output->history_messages) {
    if (message.role != wh::schema::message_role::tool) {
      continue;
    }
    saw_tool_message = true;
    REQUIRE(message.tool_call_id == "call-lookup-1");
    REQUIRE(message.tool_name == "lookup");
  }
  REQUIRE(saw_tool_message);

  auto final_iter = output->output_values.find("final");
  REQUIRE(final_iter != output->output_values.end());
  auto *final_text = wh::core::any_cast<std::string>(&final_iter->second);
  REQUIRE(final_text != nullptr);
  REQUIRE(*final_text == "tool loop done");

  REQUIRE(state->stream_calls == 2U);
}
