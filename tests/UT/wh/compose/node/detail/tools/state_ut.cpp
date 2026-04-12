#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wh/compose/node/detail/tools/state.hpp"

namespace {

[[nodiscard]] auto make_tool_batch_value(std::vector<wh::compose::tool_call> calls)
    -> wh::compose::graph_value {
  return wh::compose::graph_value{
      wh::compose::tool_batch{.calls = std::move(calls)}};
}

[[nodiscard]] auto make_registry(bool alpha_direct, bool beta_direct)
    -> wh::compose::tool_registry {
  wh::compose::tool_registry registry{};
  registry.emplace("alpha", wh::compose::tool_entry{.return_direct = alpha_direct});
  registry.emplace("beta", wh::compose::tool_entry{.return_direct = beta_direct});
  return registry;
}

} // namespace

TEST_CASE("tools state helpers cover batch extraction id shaping overrides and context projection",
          "[UT][wh/compose/node/detail/tools/state.hpp][resolve_tools_state][condition][branch][boundary]") {
  REQUIRE(wh::compose::detail::resolve_parallel_call_budget(0U, 0U) == 0U);
  REQUIRE(wh::compose::detail::resolve_parallel_call_budget(3U, 0U) == 3U);
  REQUIRE(wh::compose::detail::resolve_parallel_call_budget(5U, 2U) == 2U);

  wh::compose::tool_call explicit_id{};
  explicit_id.call_id = "call-7";
  explicit_id.tool_name = "alpha";
  REQUIRE(wh::compose::detail::resolve_call_id(explicit_id, 9U) == "call-7");

  wh::compose::tool_call generated_id{};
  generated_id.tool_name = "beta";
  REQUIRE(wh::compose::detail::resolve_call_id(generated_id, 2U) == "beta#2");

  auto type_mismatch =
      wh::compose::detail::extract_calls(wh::compose::graph_value{1});
  REQUIRE(type_mismatch.has_error());
  REQUIRE(type_mismatch.error() == wh::core::errc::type_mismatch);

  auto empty_batch = wh::compose::detail::extract_calls(
      make_tool_batch_value(std::vector<wh::compose::tool_call>{}));
  REQUIRE(empty_batch.has_error());
  REQUIRE(empty_batch.error() == wh::core::errc::invalid_argument);

  auto missing_name = wh::compose::detail::extract_calls(make_tool_batch_value(
      std::vector<wh::compose::tool_call>{{.call_id = "c", .tool_name = ""}}));
  REQUIRE(missing_name.has_error());
  REQUIRE(missing_name.error() == wh::core::errc::invalid_argument);

  auto extract_input = make_tool_batch_value(std::vector<wh::compose::tool_call>{
      {.call_id = "a", .tool_name = "alpha"},
      {.tool_name = "beta"},
  });
  auto extracted = wh::compose::detail::extract_calls(extract_input);
  REQUIRE(extracted.has_value());
  REQUIRE(extracted.value().size() == 2U);
  REQUIRE(extracted.value()[0].get().tool_name == "alpha");
  REQUIRE(extracted.value()[1].get().tool_name == "beta");

  auto base_registry = make_registry(false, false);
  auto override_registry = make_registry(true, false);
  wh::compose::tools_options options{};
  options.missing = wh::compose::tool_entry{.return_direct = true};
  options.sequential = true;
  wh::compose::tools_rerun rerun{};

  wh::compose::graph_call_options call_options_storage{};
  call_options_storage.tools = wh::compose::tools_call_options{
      .registry = std::cref(override_registry),
      .sequential = false,
      .rerun = &rerun,
  };
  wh::compose::graph_call_scope scope{call_options_storage};

  wh::compose::node_runtime runtime{};
  runtime.set_call_options(&scope);

  auto resolve_input = make_tool_batch_value(std::vector<wh::compose::tool_call>{
      {.tool_name = "alpha"},
      {.call_id = "custom", .tool_name = "beta"},
      {.tool_name = "missing"},
  });
  auto resolved = wh::compose::detail::resolve_tools_state(
      resolve_input, base_registry, options, runtime);
  REQUIRE(resolved.has_value());

  auto state = std::move(resolved).value();
  REQUIRE(state.options == &options);
  REQUIRE(state.default_tools == &base_registry);
  REQUIRE_FALSE(state.sequential);
  REQUIRE(state.shared_rerun.has_value());
  REQUIRE(&state.active_tools() == &override_registry);
  REQUIRE(&state.rerun() == &rerun);
  REQUIRE(state.has_return_direct);
  REQUIRE(state.plans.size() == 3U);
  REQUIRE(state.plans[0].call_id == "alpha#0");
  REQUIRE(state.plans[1].call_id == "custom");
  REQUIRE(state.plans[2].call_id == "missing#2");
  REQUIRE(state.execute_indices == std::vector<std::size_t>{0U, 2U});

  wh::core::run_context parent{};
  state.parent_context = &parent;
  REQUIRE(&state.base_context() == &parent);

  REQUIRE(wh::core::set_session_value(parent, "shared", 3).has_value());
  REQUIRE(wh::core::session_value_ref<int>(state.base_context(), "shared")
              .value()
              .get() == 3);

  auto *alpha = wh::compose::detail::find_tool(state, "alpha");
  REQUIRE(alpha != nullptr);
  REQUIRE(alpha->return_direct);
  auto *missing = wh::compose::detail::find_tool(state, "unknown");
  REQUIRE(missing != nullptr);
  REQUIRE(missing->return_direct);

  auto materialized = wh::compose::detail::make_call(state.plans[0]);
  REQUIRE(materialized.tool_name == "alpha");
  REQUIRE(materialized.call_id == "alpha#0");

  auto scope_view = wh::compose::detail::make_scope(materialized, parent);
  REQUIRE(scope_view.component == "tools_node");
  REQUIRE(scope_view.implementation == "tool");
  REQUIRE(scope_view.tool_name == "alpha");
  REQUIRE(scope_view.call_id == "alpha#0");
  REQUIRE(scope_view.location().to_string("/") == "tool/alpha/alpha#0");

  wh::core::run_context merged_target{};
  wh::core::run_context merged_source{};
  merged_source.resume_info.emplace();
  merged_source.interrupt_info.emplace();
  merged_source.interrupt_info->interrupt_id = "interrupt";
  REQUIRE(wh::compose::detail::merge_call_context(merged_target, merged_source).has_value());
  REQUIRE(merged_target.resume_info.has_value());
  REQUIRE(merged_target.interrupt_info.has_value());
  REQUIRE(merged_target.interrupt_info->interrupt_id == "interrupt");

  auto sync_not_supported = wh::compose::detail::make_sync_tools_state(
      make_tool_batch_value(std::vector<wh::compose::tool_call>{
          {.tool_name = "alpha"},
      }),
      base_registry, options, parent, runtime);
  REQUIRE(sync_not_supported.has_error());
  REQUIRE(sync_not_supported.error() == wh::core::errc::not_supported);

  call_options_storage.tools->sequential = true;
  auto sync_state = wh::compose::detail::make_sync_tools_state(
      make_tool_batch_value(std::vector<wh::compose::tool_call>{
          {.tool_name = "alpha"},
      }),
      base_registry, options, parent, runtime);
  REQUIRE(sync_state.has_value());
  REQUIRE(sync_state.value().parent_context == &parent);
  REQUIRE_FALSE(sync_state.value().owned_context.has_value());

  parent.callbacks.emplace();
  wh::compose::graph_resolved_node_observation observation{};
  observation.callbacks_enabled = false;
  wh::compose::graph_node_trace trace{};
  trace.trace_id = "trace";
  runtime.set_observation(&observation).set_trace(&trace);
  auto async_state = wh::compose::detail::make_async_tools_state(
      make_tool_batch_value(std::vector<wh::compose::tool_call>{
          {.tool_name = "alpha"},
      }),
      base_registry, options, parent, runtime);
  REQUIRE(async_state.has_value());
  REQUIRE(async_state.value().parent_context == &parent);
  REQUIRE(async_state.value().owned_context.has_value());
  REQUIRE(async_state.value().owned_context->callbacks == std::nullopt);
}

TEST_CASE("tools state helper falls back to null missing tool when no override handler exists",
          "[UT][wh/compose/node/detail/tools/state.hpp][find_tool][condition][branch][boundary]") {
  auto registry = make_registry(false, false);
  wh::compose::tools_options options{};
  wh::compose::node_runtime runtime{};

  auto resolved = wh::compose::detail::resolve_tools_state(
      make_tool_batch_value(std::vector<wh::compose::tool_call>{
          {.tool_name = "alpha"},
      }),
      registry, options, runtime);
  REQUIRE(resolved.has_value());

  const auto &state = resolved.value();
  REQUIRE_FALSE(state.has_return_direct);
  REQUIRE(state.execute_indices == std::vector<std::size_t>{0U});
  REQUIRE(wh::compose::detail::find_tool(state, "alpha") != nullptr);
  REQUIRE(wh::compose::detail::find_tool(state, "missing") == nullptr);
}
