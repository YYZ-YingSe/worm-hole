// Defines the authored toolset wrapper used by agent-family surfaces to freeze
// tool schemas, dispatch entries, and return-direct metadata.
#pragma once

#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/tool/tool.hpp"

namespace wh::agent {

/// Registration flags for one authored tool entry.
struct tool_registration {
  /// True marks the registered tool as return-direct candidate.
  bool return_direct{false};
};

/// Explicit lowering controls for one authored toolset -> tools-node binding.
struct tools_node_authoring_options {
  /// Selected tools-node execution mode.
  wh::compose::node_exec_mode exec_mode{wh::compose::node_exec_mode::sync};
  /// True keeps tool calls sequential inside the lowered tools node.
  bool sequential{true};
};

namespace detail {

template <typename tool_t>
concept sync_invoke_tool_component =
    requires(const tool_t &tool, wh::tool::tool_request request, wh::core::run_context &context) {
      { tool.invoke(std::move(request), context) } -> std::same_as<wh::tool::tool_invoke_result>;
      { tool.schema() } -> std::same_as<const wh::schema::tool_schema_definition &>;
    };

template <typename tool_t>
concept async_invoke_tool_component =
    requires(const tool_t &tool, wh::tool::tool_request request, wh::core::run_context &context) {
      { tool.async_invoke(std::move(request), context) } -> stdexec::sender;
      { tool.schema() } -> std::same_as<const wh::schema::tool_schema_definition &>;
    };

template <typename tool_t>
concept sync_stream_tool_component =
    requires(const tool_t &tool, wh::tool::tool_request request, wh::core::run_context &context) {
      {
        tool.stream(std::move(request), context)
      } -> std::same_as<wh::tool::tool_output_stream_result>;
      { tool.schema() } -> std::same_as<const wh::schema::tool_schema_definition &>;
    };

template <typename tool_t>
concept async_stream_tool_component =
    requires(const tool_t &tool, wh::tool::tool_request request, wh::core::run_context &context) {
      { tool.async_stream(std::move(request), context) } -> stdexec::sender;
      { tool.schema() } -> std::same_as<const wh::schema::tool_schema_definition &>;
    };

template <typename tool_t>
concept invoke_tool_component =
    sync_invoke_tool_component<tool_t> || async_invoke_tool_component<tool_t>;

template <typename tool_t>
concept stream_tool_component =
    sync_stream_tool_component<tool_t> || async_stream_tool_component<tool_t>;

template <typename tool_t>
concept registered_tool_component = invoke_tool_component<tool_t> || stream_tool_component<tool_t>;

template <registered_tool_component tool_t>
[[nodiscard]] inline auto make_tool_entry(const tool_t &tool, const bool return_direct)
    -> wh::compose::tool_entry {
  wh::compose::tool_entry entry{};
  if constexpr (sync_invoke_tool_component<tool_t>) {
    entry.invoke = wh::compose::tool_invoke{
        [tool](const wh::compose::tool_call &call,
               wh::tool::call_scope scope) -> wh::core::result<wh::compose::graph_value> {
          auto status =
              tool.invoke(wh::tool::tool_request{.input_json = call.arguments}, scope.run);
          if (status.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(status.error());
          }
          return wh::compose::graph_value{std::move(status).value()};
        }};
  }
  if constexpr (async_invoke_tool_component<tool_t>) {
    entry.async_invoke = wh::compose::tool_async_invoke{
        [tool](wh::compose::tool_call call,
               wh::tool::call_scope scope) -> wh::compose::tools_invoke_sender {
          return tool.async_invoke(wh::tool::tool_request{.input_json = std::move(call.arguments)},
                                   scope.run) |
                 stdexec::then([](wh::tool::tool_invoke_result status)
                                   -> wh::core::result<wh::compose::graph_value> {
                   if (status.has_error()) {
                     return wh::core::result<wh::compose::graph_value>::failure(status.error());
                   }
                   return wh::compose::graph_value{std::move(status).value()};
                 });
        }};
  }
  if constexpr (sync_stream_tool_component<tool_t>) {
    entry.stream = wh::compose::tool_stream{
        [tool](const wh::compose::tool_call &call,
               wh::tool::call_scope scope) -> wh::core::result<wh::compose::graph_stream_reader> {
          auto status =
              tool.stream(wh::tool::tool_request{.input_json = call.arguments}, scope.run);
          if (status.has_error()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(status.error());
          }
          return wh::compose::to_graph_stream_reader(std::move(status).value());
        }};
  }
  if constexpr (async_stream_tool_component<tool_t>) {
    entry.async_stream = wh::compose::tool_async_stream{
        [tool](wh::compose::tool_call call,
               wh::tool::call_scope scope) -> wh::compose::tools_stream_sender {
          return tool.async_stream(wh::tool::tool_request{.input_json = std::move(call.arguments)},
                                   scope.run) |
                 stdexec::then([](wh::tool::tool_output_stream_result status)
                                   -> wh::core::result<wh::compose::graph_stream_reader> {
                   if (status.has_error()) {
                     return wh::core::result<wh::compose::graph_stream_reader>::failure(
                         status.error());
                   }
                   return wh::compose::to_graph_stream_reader(std::move(status).value());
                 });
        }};
  }
  entry.return_direct = return_direct;
  return entry;
}

} // namespace detail

/// Frozen authored toolset used by one agent-family surface.
class toolset {
public:
  toolset() = default;

  toolset(const toolset &) = default;
  toolset(toolset &&) noexcept = default;
  auto operator=(const toolset &) -> toolset & = default;
  auto operator=(toolset &&) noexcept -> toolset & = default;
  ~toolset() = default;

  /// Registers one raw compose tool entry plus its public tool schema.
  auto add_entry(wh::schema::tool_schema_definition schema, wh::compose::tool_entry entry,
                 const tool_registration registration = {}) -> wh::core::result<void> {
    if (schema.name.empty() || schema.description.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!static_cast<bool>(entry.invoke) && !static_cast<bool>(entry.stream) &&
        !static_cast<bool>(entry.async_invoke) && !static_cast<bool>(entry.async_stream)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (registry_.contains(schema.name)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    entry.return_direct = registration.return_direct;
    if (registration.return_direct) {
      return_direct_names_.insert(schema.name);
    }
    schemas_.push_back(schema);
    registry_.insert_or_assign(std::move(schema.name), std::move(entry));
    return {};
  }

  /// Registers one executable tool component and derives its dispatch entry
  /// from the stable tool contract.
  template <detail::registered_tool_component tool_t>
  auto add_tool(const tool_t &tool, const tool_registration registration = {})
      -> wh::core::result<void> {
    return add_entry(tool.schema(), detail::make_tool_entry(tool, registration.return_direct),
                     registration);
  }

  /// Appends one shared tool middleware layer in declaration order.
  auto add_middleware(wh::compose::tool_middleware middleware) -> wh::core::result<void> {
    runtime_options_.middleware.push_back(std::move(middleware));
    return {};
  }

  /// Returns true when no tools have been registered.
  [[nodiscard]] auto empty() const noexcept -> bool { return schemas_.empty(); }

  /// Returns the current registered tool count.
  [[nodiscard]] auto size() const noexcept -> std::size_t { return schemas_.size(); }

  /// Returns the public tool schema set bound to the model request.
  [[nodiscard]] auto schemas() const noexcept
      -> std::span<const wh::schema::tool_schema_definition> {
    return {schemas_.data(), schemas_.size()};
  }

  /// Returns the compose tool registry used for tool dispatch.
  [[nodiscard]] auto registry() const noexcept -> const wh::compose::tool_registry & {
    return registry_;
  }

  /// Returns the compose tools-node runtime options used by this toolset.
  [[nodiscard]] auto runtime_options() const noexcept -> const wh::compose::tools_options & {
    return runtime_options_;
  }

  /// Pins the authored tools-node lowering options used by this toolset.
  auto set_node_options(const tools_node_authoring_options options) -> wh::core::result<void> {
    runtime_options_.sequential = options.sequential;
    node_options_ = options;
    return {};
  }

  /// Returns the authored tools-node lowering options when already pinned.
  [[nodiscard]] auto node_options() const noexcept -> std::optional<tools_node_authoring_options> {
    return node_options_;
  }

  /// Returns true when the named tool is configured as return-direct.
  [[nodiscard]] auto is_return_direct_tool(const std::string_view tool_name) const noexcept
      -> bool {
    return return_direct_names_.contains(tool_name);
  }

private:
  /// Public tool schemas bound into model requests.
  std::vector<wh::schema::tool_schema_definition> schemas_{};
  /// Compose dispatch endpoints keyed by stable tool name.
  wh::compose::tool_registry registry_{};
  /// Shared middleware and missing-tool behavior passed to tools-node runtime.
  wh::compose::tools_options runtime_options_{};
  /// Optional authored tools-node lowering options.
  std::optional<tools_node_authoring_options> node_options_{};
  /// Set of tool names that terminate the loop once their result is observed.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      return_direct_names_{};
};

} // namespace wh::agent
