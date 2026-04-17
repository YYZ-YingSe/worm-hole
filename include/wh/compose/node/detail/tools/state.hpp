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
#include "wh/core/intrusive_ptr.hpp"

namespace wh::compose {
namespace detail {

[[nodiscard]] inline auto resolve_parallel_call_budget(const std::size_t total,
                                                       const std::size_t parallel_gate) noexcept
    -> std::size_t {
  if (total == 0U) {
    return 0U;
  }
  if (parallel_gate == 0U) {
    return total;
  }
  return std::min(total, parallel_gate);
}

[[nodiscard]] inline auto resolve_call_id(const tool_call &call, const std::size_t index)
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
    return wh::core::result<std::vector<std::reference_wrapper<const tool_call>>>::failure(
        wh::core::errc::type_mismatch);
  }

  std::vector<std::reference_wrapper<const tool_call>> calls{};
  calls.reserve(batch->calls.size());
  for (const auto &call : batch->calls) {
    if (call.tool_name.empty()) {
      return wh::core::result<std::vector<std::reference_wrapper<const tool_call>>>::failure(
          wh::core::errc::invalid_argument);
    }
    calls.push_back(call);
  }
  if (calls.empty()) {
    return wh::core::result<std::vector<std::reference_wrapper<const tool_call>>>::failure(
        wh::core::errc::invalid_argument);
  }
  return calls;
}

using tool_after = wh::core::callback_function<wh::core::result<void>(
    const tool_call &, graph_value &, const wh::tool::call_scope &) const>;

struct tool_after_chain final
    : wh::core::detail::intrusive_enable_from_this<tool_after_chain> {
  std::vector<tool_after> handlers{};

  tool_after_chain() = default;

  explicit tool_after_chain(std::vector<tool_after> handlers_value)
      : handlers(std::move(handlers_value)) {}
};

using tool_after_chain_ptr =
    wh::core::detail::intrusive_ptr<const tool_after_chain>;

[[nodiscard]] inline auto make_tool_after_chain(const tools_options &options)
    -> tool_after_chain_ptr {
  std::vector<tool_after> handlers{};
  handlers.reserve(options.middleware.size());
  for (const auto &middleware : options.middleware) {
    if (!static_cast<bool>(middleware.after)) {
      continue;
    }
    handlers.push_back(middleware.after);
  }
  if (handlers.empty()) {
    return {};
  }
  return wh::core::detail::make_intrusive<tool_after_chain>(
      std::move(handlers));
}

[[nodiscard]] inline auto has_tool_afters(
    const tool_after_chain_ptr &afters) noexcept -> bool {
  return static_cast<bool>(afters);
}

[[nodiscard]] inline auto run_after(const tool_after_chain_ptr &afters,
                                    const tool_call &call, graph_value &value,
                                    const wh::tool::call_scope &scope)
    -> wh::core::result<void> {
  if (!afters) {
    return {};
  }
  for (const auto &after : afters->handlers) {
    auto status = after(call, value, scope);
    if (status.has_error()) {
      return wh::core::result<void>::failure(status.error());
    }
  }
  return {};
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
  std::string call_id{};
  graph_stream_reader stream{};
  graph_value rerun_extra{};
};

struct tools_state {
  wh::core::run_context *parent_context{nullptr};
  std::optional<wh::core::run_context> owned_context{};
  const tools_options *options{nullptr};
  const tool_registry *default_tools{nullptr};
  std::optional<std::reference_wrapper<const tool_registry>> override_tools{};
  tools_rerun local_rerun{};
  std::optional<std::reference_wrapper<tools_rerun>> shared_rerun{};
  tool_after_chain_ptr afters{};
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

  [[nodiscard]] auto base_context() const noexcept -> const wh::core::run_context & {
    if (owned_context.has_value()) {
      return *owned_context;
    }
    return *parent_context;
  }
};

[[nodiscard]] inline auto resolve_tools_state(const graph_value &input, const tool_registry &tools,
                                              const tools_options &options,
                                              const node_runtime &runtime)
    -> wh::core::result<tools_state> {
  auto calls = extract_calls(input);
  if (calls.has_error()) {
    return wh::core::result<tools_state>::failure(calls.error());
  }

  tools_state state{};
  state.options = std::addressof(options);
  state.default_tools = std::addressof(tools);
  state.afters = make_tool_after_chain(options);
  state.sequential = options.sequential;

  if (runtime.call_options() != nullptr && runtime.call_options()->tools().has_value()) {
    const auto &tool_options = *runtime.call_options()->tools();
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

[[nodiscard]] inline auto
make_sync_tools_state(const graph_value &input, const tool_registry &tools,
                      const tools_options &options, wh::core::run_context &context,
                      const node_runtime &runtime) -> wh::core::result<tools_state> {
  auto resolved = resolve_tools_state(input, tools, options, runtime);
  if (resolved.has_error()) {
    return resolved;
  }
  auto state = std::move(resolved).value();
  state.parent_context = std::addressof(context);
  if (!state.sequential) {
    return wh::core::result<tools_state>::failure(wh::core::errc::not_supported);
  }
  return state;
}

[[nodiscard]] inline auto
make_async_tools_state(const graph_value &input, const tool_registry &tools,
                       const tools_options &options, wh::core::run_context &context,
                       const node_runtime &runtime) -> wh::core::result<tools_state> {
  auto resolved = resolve_tools_state(input, tools, options, runtime);
  if (resolved.has_error()) {
    return resolved;
  }
  auto state = std::move(resolved).value();
  state.parent_context = std::addressof(context);
  auto node_context = make_node_context(context, node_observation(runtime), node_trace(runtime));
  if (node_context.has_error()) {
    return wh::core::result<tools_state>::failure(node_context.error());
  }
  auto prepared_context = std::move(node_context).value();
  if (prepared_context.has_value()) {
    state.owned_context = std::move(*prepared_context);
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

[[nodiscard]] inline auto make_scope(tool_call &call, wh::core::run_context &context)
    -> wh::tool::call_scope {
  return wh::tool::call_scope{
      .run = context,
      .component = "tools_node",
      .implementation = "tool",
      .tool_name = call.tool_name,
      .call_id = call.call_id,
  };
}

inline auto merge_call_context(wh::core::run_context &target, const wh::core::run_context &source)
    -> wh::core::result<void> {
  if (source.resume_info.has_value()) {
    auto owned_resume = wh::core::into_owned(*source.resume_info);
    if (owned_resume.has_error()) {
      return wh::core::result<void>::failure(owned_resume.error());
    }
    target.resume_info = std::move(owned_resume).value();
  } else {
    target.resume_info.reset();
  }

  if (source.interrupt_info.has_value()) {
    auto owned_interrupt = wh::core::into_owned(*source.interrupt_info);
    if (owned_interrupt.has_error()) {
      return wh::core::result<void>::failure(owned_interrupt.error());
    }
    target.interrupt_info = std::move(owned_interrupt).value();
  } else {
    target.interrupt_info.reset();
  }

  return {};
}

} // namespace detail
} // namespace wh::compose
