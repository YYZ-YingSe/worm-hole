#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/component_contract_support.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/node/compiled.hpp"
#include "wh/compose/node/component.hpp"
#include "wh/model/echo_chat_model.hpp"
#include "wh/model/fallback_chat_model.hpp"
#include "wh/tool/catalog.hpp"

namespace {

using wh::testing::helper::make_user_message;
using wh::testing::helper::read_any;
using wh::testing::helper::register_test_callbacks;
using wh::testing::helper::take_try_chunk;
using wh::testing::helper::tool_binding_probe_model;
using wh::testing::helper::tool_binding_probe_model_impl;
using wh::testing::helper::tool_binding_probe_state;

} // namespace

TEST_CASE("echo chat model supports invoke stream and immutable tool binding",
          "[core][model][functional]") {
  wh::model::echo_chat_model model{};
  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  wh::core::run_context callback_context{};

  const auto invoke_result = model.invoke(request, callback_context);
  REQUIRE(invoke_result.has_value());
  REQUIRE(invoke_result.value().message.role == wh::schema::message_role::assistant);
  REQUIRE(std::get<wh::schema::text_part>(invoke_result.value().message.parts.front()).text ==
          "hello");

  auto stream_result = model.stream(request, callback_context);
  REQUIRE(stream_result.has_value());
  auto reader = std::move(stream_result).value();
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(first.value().value->role == wh::schema::message_role::assistant);
  auto second = take_try_chunk(reader);
  REQUIRE(second.has_value());
  REQUIRE(second.value().eof);

  wh::schema::tool_schema_definition tool_schema{};
  tool_schema.name = "weather";
  auto bound =
      model.bind_tools(std::span<const wh::schema::tool_schema_definition>{&tool_schema, 1U});
  REQUIRE(bound.bound_tools().size() == 1U);
  REQUIRE(model.bound_tools().empty());
}

TEST_CASE("model fallback route freeze and structured output negotiation",
          "[core][model][functional]") {
  wh::model::chat_model_common_options options{};
  options.model_id = "ToolBindingProbeModel";
  options.selection_policy = wh::model::model_selection_policy::cost_first;
  options.fallback.ordered_candidates = {"backup-a", "backup-b"};
  auto frozen = wh::model::freeze_model_candidates(
      options, std::array<std::string, 2U>{"discovered-a", "discovered-b"});
  REQUIRE_FALSE(frozen.empty());
  REQUIRE(frozen.front() == "backup-b");

  auto negotiated = wh::model::negotiate_structured_output(
      wh::model::structured_output_policy{
          wh::model::structured_output_preference::provider_native_first, true},
      false, true);
  REQUIRE_FALSE(negotiated.use_provider_native);
  REQUIRE(negotiated.use_tool_call_fallback);

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("fallback"));
  request.tools.push_back(wh::schema::tool_schema_definition{"weather", "desc"});
  wh::model::chat_model_options request_options{};
  request_options.set_base(options);
  request.options = request_options;

  auto state = std::make_shared<tool_binding_probe_state>();
  const std::array<tool_binding_probe_model, 1U> candidates{
      tool_binding_probe_model{tool_binding_probe_model_impl{state}}};
  auto fallback_report = wh::model::invoke_with_fallback(
      std::span<const tool_binding_probe_model>{candidates}, request);
  REQUIRE(fallback_report.has_value());
  REQUIRE(fallback_report.value().response.message.role == wh::schema::message_role::assistant);
  REQUIRE_FALSE(fallback_report.value().frozen_candidates.empty());
  REQUIRE(std::find(fallback_report.value().frozen_candidates.begin(),
                    fallback_report.value().frozen_candidates.end(),
                    "ToolBindingProbeModel") != fallback_report.value().frozen_candidates.end());
  REQUIRE_FALSE(fallback_report.value().structured_output.use_provider_native);
  REQUIRE(fallback_report.value().structured_output.use_tool_call_fallback);
  REQUIRE(state->bind_calls == 1U);
  REQUIRE(state->invoke_calls == 1U);
  REQUIRE(state->last_bound_tool_count == 1U);
}

TEST_CASE("model fallback report keeps failure chain and tool-choice semantics",
          "[core][model][functional]") {
  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("fallback"));
  request.tools.push_back(wh::schema::tool_schema_definition{"weather", "desc"});
  request.tools.push_back(wh::schema::tool_schema_definition{"search", "desc"});

  auto state = std::make_shared<tool_binding_probe_state>();
  const std::array<tool_binding_probe_model, 1U> candidates{
      tool_binding_probe_model{tool_binding_probe_model_impl{state}}};

  wh::model::chat_model_common_options disable_options{};
  disable_options.tool_choice.mode = wh::schema::tool_call_mode::disable;
  request.options.set_base(disable_options);
  auto disabled = wh::model::invoke_with_fallback_report_only(
      std::span<const tool_binding_probe_model>{candidates}, request);
  REQUIRE(disabled.final_error.has_value());
  REQUIRE(*disabled.final_error == wh::core::errc::invalid_argument);
  REQUIRE(disabled.attempts.size() == 1U);
  REQUIRE(state->bind_calls == 0U);

  wh::model::chat_model_common_options force_options{};
  force_options.tool_choice.mode = wh::schema::tool_call_mode::force;
  force_options.allowed_tool_names = {"weather"};
  request.options.set_base(force_options);
  auto forced = wh::model::invoke_with_fallback_report_only(
      std::span<const tool_binding_probe_model>{candidates}, request);
  REQUIRE_FALSE(forced.final_error.has_value());
  REQUIRE(forced.response.message.role == wh::schema::message_role::assistant);
  REQUIRE(state->bind_calls == 1U);
  REQUIRE(state->last_bound_tool_count == 1U);

  wh::model::chat_model_common_options force_not_found{};
  force_not_found.tool_choice.mode = wh::schema::tool_call_mode::force;
  force_not_found.allowed_tool_names = {"missing"};
  request.options.set_base(force_not_found);
  auto missing_tool = wh::model::invoke_with_fallback_report_only(
      std::span<const tool_binding_probe_model>{candidates}, request);
  REQUIRE(missing_tool.final_error.has_value());
  REQUIRE(*missing_tool.final_error == wh::core::errc::not_found);

  wh::model::chat_request no_tools_request = request;
  no_tools_request.tools.clear();
  no_tools_request.options.set_base(force_options);
  auto no_tools_forced = wh::model::invoke_with_fallback_report_only(
      std::span<const tool_binding_probe_model>{candidates}, no_tools_request);
  REQUIRE(no_tools_forced.final_error.has_value());
  REQUIRE(*no_tools_forced.final_error == wh::core::errc::invalid_argument);

  wh::model::chat_request invalid_request{};
  auto report = wh::model::invoke_with_fallback_report_only(
      std::span<const tool_binding_probe_model>{}, invalid_request);
  REQUIRE(report.final_error.has_value());
  REQUIRE(*report.final_error == wh::core::errc::not_found);
  REQUIRE(report.attempts.empty());
}

TEST_CASE("model fallback report can disable per-attempt audit chain",
          "[core][model][functional]") {
  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("fallback"));
  wh::model::chat_model_common_options options{};
  options.fallback.keep_failure_reasons = false;
  request.options.set_base(options);

  auto state = std::make_shared<tool_binding_probe_state>();
  const std::array<tool_binding_probe_model, 1U> candidates{
      tool_binding_probe_model{tool_binding_probe_model_impl{state}}};
  auto report = wh::model::invoke_with_fallback_report_only(
      std::span<const tool_binding_probe_model>{candidates}, request);
  REQUIRE(report.final_error.has_value());
  REQUIRE(*report.final_error == wh::core::errc::invalid_argument);
  REQUIRE(report.attempts.empty());
}

TEST_CASE("model stream fallback binds tools and exposes selected reader",
          "[core][model][functional]") {
  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("fallback"));
  request.tools.push_back(wh::schema::tool_schema_definition{"weather", "desc"});

  auto state = std::make_shared<tool_binding_probe_state>();
  const std::array<tool_binding_probe_model, 1U> candidates{
      tool_binding_probe_model{tool_binding_probe_model_impl{state}}};

  auto fallback_report = wh::model::stream_with_fallback(
      std::span<const tool_binding_probe_model>{candidates}, request);
  REQUIRE(fallback_report.has_value());
  REQUIRE(fallback_report.value().selected_model == "ToolBindingProbeModel");
  REQUIRE(state->bind_calls == 1U);
  REQUIRE(state->stream_calls == 1U);
  REQUIRE(state->last_bound_tool_count == 1U);

  auto reader = std::move(fallback_report).value().reader;
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(first.value().value->parts.front()).text ==
          "bound-stream");
  auto second = take_try_chunk(reader);
  REQUIRE(second.has_value());
  REQUIRE(second.value().eof);
}

TEST_CASE("fallback chat model resolves catalog tools for invoke and stream",
          "[core][model][functional]") {
  wh::schema::tool_schema_definition tool_schema{};
  tool_schema.name = "catalog_tool";
  tool_schema.description = "catalog bound tool";

  std::size_t handshake_calls = 0U;
  std::size_t fetch_calls = 0U;
  wh::tool::tool_catalog_cache cache{wh::tool::tool_catalog_source{
      .handshake = [&handshake_calls]() -> wh::core::result<void> {
        ++handshake_calls;
        return {};
      },
      .fetch_catalog = [tool_schema, &fetch_calls]()
          -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> {
        ++fetch_calls;
        return std::vector<wh::schema::tool_schema_definition>{tool_schema};
      },
  }};

  auto state = std::make_shared<tool_binding_probe_state>();
  wh::model::fallback_chat_model<tool_binding_probe_model> wrapper{
      std::vector<tool_binding_probe_model>{
          tool_binding_probe_model{tool_binding_probe_model_impl{state}}},
      std::move(cache)};

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("catalog fallback"));
  wh::core::run_context context{};

  auto invoked = wrapper.invoke(request, context);
  REQUIRE(invoked.has_value());
  REQUIRE(std::get<wh::schema::text_part>(invoked.value().message.parts.front()).text == "bound");
  REQUIRE(handshake_calls == 1U);
  REQUIRE(fetch_calls == 1U);
  REQUIRE(state->bind_calls == 1U);
  REQUIRE(state->invoke_calls == 1U);

  auto streamed = wrapper.stream(request, context);
  REQUIRE(streamed.has_value());
  REQUIRE(handshake_calls == 1U);
  REQUIRE(fetch_calls == 1U);
  REQUIRE(state->bind_calls == 2U);
  REQUIRE(state->stream_calls == 1U);

  auto reader = std::move(streamed).value();
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(first.value().value->parts.front()).text ==
          "bound-stream");
  auto second = take_try_chunk(reader);
  REQUIRE(second.has_value());
  REQUIRE(second.value().eof);
}

TEST_CASE("fallback chat model binds compose model nodes for value and stream",
          "[core][model][functional]") {
  wh::schema::tool_schema_definition tool_schema{};
  tool_schema.name = "compose_tool";
  tool_schema.description = "compose bound tool";

  auto state = std::make_shared<tool_binding_probe_state>();
  const auto wrapper =
      wh::model::fallback_chat_model<tool_binding_probe_model>{
          std::vector<tool_binding_probe_model>{
              tool_binding_probe_model{tool_binding_probe_model_impl{state}}}}
          .bind_tools(std::span<const wh::schema::tool_schema_definition>{&tool_schema, 1U});

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("compose fallback"));

  auto invoke_node = wh::compose::make_component_node<wh::compose::component_kind::model,
                                                      wh::compose::node_contract::value,
                                                      wh::compose::node_contract::value>(
      "fallback-invoke", wrapper);
  auto stream_node = wh::compose::make_component_node<wh::compose::component_kind::model,
                                                      wh::compose::node_contract::value,
                                                      wh::compose::node_contract::stream>(
      "fallback-stream", wrapper);

  wh::compose::graph invoke_graph{};
  REQUIRE(invoke_graph.add_component(std::move(invoke_node)).has_value());
  REQUIRE(invoke_graph.add_entry_edge("fallback-invoke").has_value());
  REQUIRE(invoke_graph.add_exit_edge("fallback-invoke").has_value());
  REQUIRE(invoke_graph.compile().has_value());

  wh::compose::graph stream_graph{};
  REQUIRE(stream_graph.add_component(std::move(stream_node)).has_value());
  REQUIRE(stream_graph.add_entry_edge("fallback-stream").has_value());
  REQUIRE(stream_graph.add_exit_edge("fallback-stream").has_value());
  REQUIRE(stream_graph.compile().has_value());

  auto invoke_compiled = invoke_graph.compiled_node_by_key("fallback-invoke");
  REQUIRE(invoke_compiled.has_value());
  auto stream_compiled = stream_graph.compiled_node_by_key("fallback-stream");
  REQUIRE(stream_compiled.has_value());

  wh::core::run_context context{};
  wh::compose::node_runtime runtime{};

  auto invoke_input = wh::compose::graph_value{wh::core::any(request)};
  auto invoked = wh::compose::run_compiled_sync_node(invoke_compiled.value().get(), invoke_input,
                                                     context, runtime);
  REQUIRE(invoked.has_value());
  auto invoke_output = read_any<wh::model::chat_response>(std::move(invoked).value());
  REQUIRE(invoke_output.has_value());
  REQUIRE(std::get<wh::schema::text_part>(invoke_output.value().message.parts.front()).text ==
          "bound");

  auto stream_input = wh::compose::graph_value{wh::core::any(std::move(request))};
  auto streamed = wh::compose::run_compiled_sync_node(stream_compiled.value().get(), stream_input,
                                                      context, runtime);
  REQUIRE(streamed.has_value());
  auto stream_output = read_any<wh::compose::graph_stream_reader>(std::move(streamed).value());
  REQUIRE(stream_output.has_value());
  auto first = stream_output.value().read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  auto message = read_any<wh::schema::message>(std::move(*first.value().value));
  REQUIRE(message.has_value());
  REQUIRE(std::get<wh::schema::text_part>(message.value().parts.front()).text == "bound-stream");

  REQUIRE(state->bind_calls == 2U);
  REQUIRE(state->invoke_calls == 1U);
  REQUIRE(state->stream_calls == 1U);
}

TEST_CASE("model callbacks bridge typed and payload for invoke and stream",
          "[core][model][functional]") {
  wh::model::echo_chat_model model{};
  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("callback"));

  std::atomic<int> started{0};
  std::atomic<int> ended{0};
  std::atomic<int> failed{0};
  std::atomic<bool> stream_seen{false};
  std::atomic<bool> payload_seen{false};

  wh::model::chat_model_common_options common{};
  common.model_id = "echo";
  request.options.set_base(common);

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context), [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::model::chat_model_callback_event>();
        REQUIRE(typed != nullptr);
        if (stage == wh::core::callback_stage::start) {
          if (typed->stream_path) {
            stream_seen.store(true, std::memory_order_release);
          }
          started.fetch_add(1, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          REQUIRE(typed->usage.total_tokens == 2U);
          ended.fetch_add(1, std::memory_order_release);
          payload_seen.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          failed.fetch_add(1, std::memory_order_release);
        }
      },
      "model-events");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  auto invoked = model.invoke(request, callback_context);
  REQUIRE(invoked.has_value());
  auto streamed = model.stream(request, callback_context);
  REQUIRE(streamed.has_value());

  wh::model::chat_request invalid_request{};
  invalid_request.options = request.options;
  auto invalid = model.invoke(invalid_request, callback_context);
  REQUIRE(invalid.has_error());

  REQUIRE(started.load(std::memory_order_acquire) == 3);
  REQUIRE(ended.load(std::memory_order_acquire) == 2);
  REQUIRE(failed.load(std::memory_order_acquire) == 1);
  REQUIRE(stream_seen.load(std::memory_order_acquire));
  REQUIRE(payload_seen.load(std::memory_order_acquire));
}
