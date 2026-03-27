// Defines tools-node runtime state shaping and invoke-time planning.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/detail/context.hpp"
#include "wh/compose/node/tools_contract.hpp"

namespace wh::compose {
namespace detail {

[[nodiscard]] inline auto resolve_parallel_call_budget(
    const std::size_t total, const std::size_t parallel_gate) noexcept
    -> std::size_t {
  if (total == 0U) {
    return 0U;
  }
  if (parallel_gate == 0U) {
    return total;
  }
  return std::min(total, parallel_gate);
}

[[nodiscard]] inline auto resolve_call_id(const tool_call &call,
                                          const std::size_t index)
    -> std::string {
  if (!call.call_id.empty()) {
    return call.call_id;
  }
  return call.tool_name + "#" + std::to_string(index);
}

[[nodiscard]] inline auto extract_calls(const graph_value &input)
    -> wh::core::result<std::vector<std::reference_wrapper<const tool_call>>> {
  const auto *batch = wh::core::any_cast<tool_batch>(&input);
  if (batch == nullptr) {
    return wh::core::result<
        std::vector<std::reference_wrapper<const tool_call>>>::failure(
        wh::core::errc::type_mismatch);
  }

  std::vector<std::reference_wrapper<const tool_call>> calls{};
  calls.reserve(batch->calls.size());
  for (const auto &call : batch->calls) {
    if (call.tool_name.empty()) {
      return wh::core::result<
          std::vector<std::reference_wrapper<const tool_call>>>::failure(
          wh::core::errc::invalid_argument);
    }
    calls.push_back(call);
  }
  if (calls.empty()) {
    return wh::core::result<
        std::vector<std::reference_wrapper<const tool_call>>>::failure(
        wh::core::errc::invalid_argument);
  }
  return calls;
}

struct call_plan {
  std::size_t index{0U};
  const tool_call *call{nullptr};
  std::string call_id{};
  bool return_direct{false};
};

struct call_completion {
  std::size_t index{0U};
  tool_call call{};
  graph_value value{};
  graph_value rerun_extra{};
};

struct stream_completion {
  std::size_t index{0U};
  tool_call call{};
  graph_stream_reader stream{};
  graph_value rerun_extra{};
  wh::core::run_context context{};
};

struct tools_state {
  wh::core::run_context *parent_context{nullptr};
  std::optional<wh::core::run_context> owned_context{};
  const tools_options *options{nullptr};
  const tool_registry *default_tools{nullptr};
  std::optional<std::reference_wrapper<const tool_registry>> override_tools{};
  tools_rerun local_rerun{};
  std::optional<std::reference_wrapper<tools_rerun>> shared_rerun{};
  bool sequential{true};
  bool has_return_direct{false};
  std::vector<call_plan> plans{};
  std::vector<std::size_t> execute_indices{};

  [[nodiscard]] auto active_tools() const noexcept -> const tool_registry & {
    if (override_tools.has_value()) {
      return override_tools->get();
    }
    return *default_tools;
  }

  [[nodiscard]] auto rerun() noexcept -> tools_rerun & {
    if (shared_rerun.has_value()) {
      return shared_rerun->get();
    }
    return local_rerun;
  }

  [[nodiscard]] auto rerun() const noexcept -> const tools_rerun & {
    if (shared_rerun.has_value()) {
      return shared_rerun->get();
    }
    return local_rerun;
  }

  [[nodiscard]] auto base_context() noexcept -> wh::core::run_context & {
    if (owned_context.has_value()) {
      return *owned_context;
    }
    return *parent_context;
  }

  [[nodiscard]] auto base_context() const noexcept
      -> const wh::core::run_context & {
    if (owned_context.has_value()) {
      return *owned_context;
    }
    return *parent_context;
  }
};

[[nodiscard]] inline auto resolve_tools_state(
    const graph_value &input, const tool_registry &tools,
    const tools_options &options, const node_runtime &runtime)
    -> wh::core::result<tools_state> {
  auto calls = extract_calls(input);
  if (calls.has_error()) {
    return wh::core::result<tools_state>::failure(calls.error());
  }

  tools_state state{};
  state.options = std::addressof(options);
  state.default_tools = std::addressof(tools);
  state.sequential = options.sequential;

  if (runtime.call_options != nullptr && runtime.call_options->tools().has_value()) {
    const auto &tool_options = *runtime.call_options->tools();
    if (tool_options.registry.has_value()) {
      state.override_tools = tool_options.registry;
    }
    if (tool_options.sequential.has_value()) {
      state.sequential = *tool_options.sequential;
    }
    if (tool_options.rerun != nullptr) {
      state.shared_rerun = std::ref(*tool_options.rerun);
    }
  }

  state.plans.resize(calls.value().size());
  for (std::size_t index = 0U; index < calls.value().size(); ++index) {
    const auto &call = calls.value()[index].get();
    auto call_id = resolve_call_id(call, index);
    bool direct = false;
    if (const auto iter = state.active_tools().find(call.tool_name);
        iter != state.active_tools().end()) {
      direct = iter->second.return_direct;
    } else if (state.options->missing.has_value()) {
      direct = state.options->missing->return_direct;
    }
    state.has_return_direct = state.has_return_direct || direct;
    state.plans[index] = call_plan{
        .index = index,
        .call = std::addressof(call),
        .call_id = std::move(call_id),
        .return_direct = direct,
    };
  }

  state.execute_indices.reserve(state.plans.size());
  for (std::size_t index = 0U; index < state.plans.size(); ++index) {
    if (!state.has_return_direct || state.plans[index].return_direct) {
      state.execute_indices.push_back(index);
    }
  }
  return state;
}

[[nodiscard]] inline auto make_sync_tools_state(
    const graph_value &input, const tool_registry &tools,
    const tools_options &options, wh::core::run_context &context,
    const node_runtime &runtime) -> wh::core::result<tools_state> {
  auto resolved = resolve_tools_state(input, tools, options, runtime);
  if (resolved.has_error()) {
    return resolved;
  }
  auto state = std::move(resolved).value();
  state.parent_context = std::addressof(context);
  if (!state.sequential) {
    return wh::core::result<tools_state>::failure(
        wh::core::errc::not_supported);
  }
  return state;
}

[[nodiscard]] inline auto make_async_tools_state(
    const graph_value &input, const tool_registry &tools,
    const tools_options &options, wh::core::run_context &context,
    const node_runtime &runtime) -> wh::core::result<tools_state> {
  auto resolved = resolve_tools_state(input, tools, options, runtime);
  if (resolved.has_error()) {
    return resolved;
  }
  auto state = std::move(resolved).value();
  state.parent_context = std::addressof(context);
  auto node_context =
      make_node_context(context, node_observation(runtime), node_trace(runtime));
  if (node_context.has_value()) {
    state.owned_context = std::move(*node_context);
  }
  return state;
}

[[nodiscard]] inline auto find_tool(const tools_state &state,
                                    const std::string_view tool_name) noexcept
    -> const tool_entry * {
  const auto iter = state.active_tools().find(tool_name);
  if (iter != state.active_tools().end()) {
    return std::addressof(iter->second);
  }
  if (state.options->missing.has_value()) {
    return std::addressof(*state.options->missing);
  }
  return nullptr;
}

[[nodiscard]] inline auto make_call(const call_plan &plan) -> tool_call {
  tool_call call = *plan.call;
  call.call_id = plan.call_id;
  return call;
}

[[nodiscard]] inline auto make_scope(tool_call &call,
                                     wh::core::run_context &context)
    -> wh::tool::call_scope {
  return wh::tool::call_scope{
      .run = context,
      .component = "tools_node",
      .implementation = "tool",
      .tool_name = call.tool_name,
      .call_id = call.call_id,
  };
}

} // namespace detail
} // namespace wh::compose
