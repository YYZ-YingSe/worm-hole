#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "wh/agent/options.hpp"

namespace {

struct probe_state {
  std::size_t bind_calls{0U};
};

class sync_probe_model_impl {
public:
  explicit sync_probe_model_impl(
      std::shared_ptr<probe_state> state,
      std::vector<wh::schema::tool_schema_definition> tools = {})
      : state_(std::move(state)), bound_tools_(std::move(tools)) {}

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"ProbeModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &,
                            wh::core::run_context &) const
      -> wh::model::chat_invoke_result {
    return wh::model::chat_response{};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &,
                            wh::core::run_context &) const
      -> wh::model::chat_message_stream_result {
    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_empty_stream_reader<wh::schema::message>()};
  }

  [[nodiscard]] auto bind_tools(
      std::span<const wh::schema::tool_schema_definition> tools) const
      -> sync_probe_model_impl {
    ++state_->bind_calls;
    return sync_probe_model_impl{
        state_,
        std::vector<wh::schema::tool_schema_definition>{tools.begin(), tools.end()}};
  }

  [[nodiscard]] auto bound_tools() const noexcept
      -> const std::vector<wh::schema::tool_schema_definition> & {
    return bound_tools_;
  }

private:
  std::shared_ptr<probe_state> state_{};
  std::vector<wh::schema::tool_schema_definition> bound_tools_{};
};

struct sync_tool {
  wh::schema::tool_schema_definition schema_value{
      .name = "search",
      .description = "lookup",
  };

  [[nodiscard]] auto schema() const
      -> const wh::schema::tool_schema_definition & {
    return schema_value;
  }

  [[nodiscard]] auto invoke(wh::tool::tool_request request,
                            wh::core::run_context &) const
      -> wh::tool::tool_invoke_result {
    return request.input_json;
  }
};

} // namespace

TEST_CASE("agent options resolve structured-output strategy to model policy",
          "[UT][wh/agent/options.hpp][resolve_structured_output_policy][condition][branch][boundary]") {
  const auto provider = wh::agent::resolve_structured_output_policy(
      wh::agent::structured_output_strategy::provider);
  REQUIRE(provider.preference ==
          wh::model::structured_output_preference::provider_native_first);
  REQUIRE_FALSE(provider.allow_tool_fallback);

  const auto tool = wh::agent::resolve_structured_output_policy(
      wh::agent::structured_output_strategy::tool);
  REQUIRE(tool.preference ==
          wh::model::structured_output_preference::tool_call_first);
  REQUIRE(tool.allow_tool_fallback);
}

TEST_CASE("agent options apply_agent_options leaves request unchanged when no structured output is requested",
          "[UT][wh/agent/options.hpp][apply_agent_options][condition][branch][boundary]") {
  wh::model::chat_request request{};
  wh::agent::agent_options options{};

  wh::agent::apply_agent_options(request, options);

  REQUIRE_FALSE(request.options.call_override().has_value());
}

TEST_CASE("agent options apply_agent_options overlays structured output onto existing request overrides",
          "[UT][wh/agent/options.hpp][apply_agent_options][condition][branch][boundary]") {
  wh::model::chat_request request{};
  request.options.set_call_override(
      wh::model::chat_model_common_options{.max_tokens = 128,
                                           .model_id = "base-model"});

  wh::agent::agent_options options{};
  options.structured_output = wh::agent::structured_output_strategy::tool;
  wh::agent::apply_agent_options(request, options);

  REQUIRE(request.options.call_override().has_value());
  REQUIRE(request.options.call_override()->max_tokens == 128U);
  REQUIRE(request.options.call_override()->model_id == "base-model");
  REQUIRE(request.options.call_override()->structured_output.allow_tool_fallback);
}

TEST_CASE("agent options resolve layered ADK call options in defaults-agent-adk-override order",
          "[UT][wh/agent/options.hpp][resolve_agent_call_options][condition][branch][boundary]") {
  wh::adk::call_options defaults{};
  wh::adk::set_global_option(defaults, "temperature", 0.1);

  wh::agent::agent_options options{};
  wh::adk::set_global_option(options.agent_controls, "temperature", 0.2);
  wh::adk::set_global_option(options.adk_controls, "temperature", 0.3);
  wh::adk::set_global_option(options.call_override, "temperature", 0.4);

  const auto resolved = wh::agent::resolve_agent_call_options(&defaults, options);

  REQUIRE(wh::adk::option_value_copy<double>(resolved.global, "temperature")
              .value() == 0.4);
}

TEST_CASE("agent options make_tool_binding_pair exposes schema and runtime entry metadata",
          "[UT][wh/agent/options.hpp][make_tool_binding_pair][condition][branch][boundary]") {
  sync_tool tool{};
  auto binding =
      wh::agent::make_tool_binding_pair(tool, {.return_direct = true});

  REQUIRE(binding.schema.name == "search");
  REQUIRE(binding.entry.return_direct);
  REQUIRE(static_cast<bool>(binding.entry.invoke));
}

TEST_CASE("agent options bind_model_tools skips empty schema lists and rebinds non-empty toolsets",
          "[UT][wh/agent/options.hpp][bind_model_tools][condition][branch][boundary]") {
  auto state = std::make_shared<probe_state>();
  sync_probe_model_impl model{state};

  REQUIRE(wh::agent::bind_model_tools(
              model,
              std::span<const wh::schema::tool_schema_definition>{})
              .has_value());
  REQUIRE(state->bind_calls == 0U);

  const std::array<wh::schema::tool_schema_definition, 1> schemas{{
      {.name = "search", .description = "lookup"},
  }};
  auto rebound = wh::agent::bind_model_tools(model, schemas);
  REQUIRE(rebound.has_value());
  REQUIRE(state->bind_calls == 1U);
  REQUIRE(rebound->bound_tools().size() == 1U);
  REQUIRE(rebound->bound_tools().front().name == "search");
}
