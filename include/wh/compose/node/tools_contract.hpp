// Defines the public tools-node contract surface.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/graph/stream.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/function.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/tool/call_scope.hpp"

namespace wh::compose {

/// One concrete tool call carried into tools-node execution.
struct tool_call {
  /// Stable correlation id for this call.
  std::string call_id{};
  /// Registered tool name.
  std::string tool_name{};
  /// Structured arguments payload encoded for the target tool.
  std::string arguments{};
  /// Optional typed bridge payload shared with internal tool adapters.
  graph_value payload{};
};

/// Batch of tool calls consumed by one tools node.
struct tool_batch {
  /// Ordered calls to execute.
  std::vector<tool_call> calls{};
};

/// One final tool result returned by `tools_node<To=value>`.
struct tool_result {
  /// Stable correlation id for this call.
  std::string call_id{};
  /// Registered tool name.
  std::string tool_name{};
  /// Tool payload emitted by the endpoint.
  graph_value value{};
};

/// One live tool event emitted by `tools_node<To=stream>`.
struct tool_event {
  /// Stable correlation id for this call.
  std::string call_id{};
  /// Registered tool name.
  std::string tool_name{};
  /// Tool payload emitted by the endpoint.
  graph_value value{};
};

/// Type-erased async invoke result boundary used by tools-node runtime.
using tools_invoke_sender = graph_value_sender;

/// Type-erased async stream result boundary used by tools-node runtime.
using tools_stream_sender = wh::core::detail::result_sender<wh::core::result<graph_stream_reader>>;

/// Tool invoke endpoint contract.
using tool_invoke = wh::core::callback_function<wh::core::result<graph_value>(
    const tool_call &, wh::tool::call_scope) const>;

/// Tool stream endpoint contract.
using tool_stream = wh::core::callback_function<wh::core::result<graph_stream_reader>(
    const tool_call &, wh::tool::call_scope) const>;

/// Async tool invoke endpoint contract.
using tool_async_invoke =
    wh::core::callback_function<tools_invoke_sender(tool_call, wh::tool::call_scope) const>;

/// Async tool stream endpoint contract.
using tool_async_stream =
    wh::core::callback_function<tools_stream_sender(tool_call, wh::tool::call_scope) const>;

/// One tool endpoint bundle used by tools node dispatch.
struct tool_entry {
  /// Invoke endpoint returning one final payload.
  tool_invoke invoke{nullptr};
  /// Stream endpoint returning live payload events.
  tool_stream stream{nullptr};
  /// Async invoke endpoint returning one final payload sender.
  tool_async_invoke async_invoke{nullptr};
  /// Async stream endpoint returning one live payload stream sender.
  tool_async_stream async_stream{nullptr};
  /// True marks this tool-call as return-direct candidate.
  bool return_direct{false};
};

/// Tool registry keyed by tool name.
using tool_registry = std::unordered_map<std::string, tool_entry, wh::core::transparent_string_hash,
                                         wh::core::transparent_string_equal>;

/// Before/after hooks shared by invoke and stream paths.
struct tool_middleware {
  /// Optional pre-call hook that may rewrite the concrete tool call.
  wh::core::callback_function<wh::core::result<void>(tool_call &, const wh::tool::call_scope &)
                                  const>
      before{nullptr};
  /// Optional post-call hook that may rewrite one emitted payload.
  wh::core::callback_function<wh::core::result<void>(const tool_call &, graph_value &,
                                                     const wh::tool::call_scope &) const>
      after{nullptr};
};

/// Tools-node runtime behavior options.
struct tools_options {
  /// Unknown-tool entry used when the requested tool is not registered.
  std::optional<tool_entry> missing{};
  /// Middleware hooks shared by invoke and stream paths.
  std::vector<tool_middleware> middleware{};
  /// True executes tool calls sequentially.
  bool sequential{true};
};

/// Rerun state shared across tools-node invocations when caller opts in.
struct tools_rerun {
  /// Tool-call ids that must be re-executed in rerun mode.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      ids{};
  /// Extra rerun metadata keyed by tool-call id.
  std::unordered_map<std::string, graph_value, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      extra{};
  /// Reusable executed tool outputs keyed by tool-call id.
  std::unordered_map<std::string, graph_value, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      outputs{};
};

/// Typed invoke-time overrides consumed by tools node.
/// This type is intentionally borrowed-oriented: registry/rerun remain
/// host-owned handles shared with the running invoke instead of owned values.
struct tools_call_options {
  /// Optional host-owned override registry borrowed for this invoke only.
  std::optional<std::reference_wrapper<const tool_registry>> registry{};
  /// Optional sequential override.
  std::optional<bool> sequential{};
  /// Optional caller-owned rerun state shared by reference with this invoke.
  tools_rerun *rerun{nullptr};
};

} // namespace wh::compose

namespace wh::compose::detail {

[[nodiscard]] inline auto into_owned_tool_call(const tool_call &call)
    -> wh::core::result<tool_call> {
  auto payload = wh::core::into_owned(call.payload);
  if (payload.has_error()) {
    return wh::core::result<tool_call>::failure(payload.error());
  }
  return tool_call{
      .call_id = call.call_id,
      .tool_name = call.tool_name,
      .arguments = call.arguments,
      .payload = std::move(payload).value(),
  };
}

[[nodiscard]] inline auto into_owned_tool_call(tool_call &&call) -> wh::core::result<tool_call> {
  auto payload = wh::core::into_owned(std::move(call.payload));
  if (payload.has_error()) {
    return wh::core::result<tool_call>::failure(payload.error());
  }
  return tool_call{
      .call_id = std::move(call.call_id),
      .tool_name = std::move(call.tool_name),
      .arguments = std::move(call.arguments),
      .payload = std::move(payload).value(),
  };
}

[[nodiscard]] inline auto into_owned_tool_batch(const tool_batch &batch)
    -> wh::core::result<tool_batch> {
  tool_batch owned_batch{};
  owned_batch.calls.reserve(batch.calls.size());
  for (const auto &call : batch.calls) {
    auto owned = into_owned_tool_call(call);
    if (owned.has_error()) {
      return wh::core::result<tool_batch>::failure(owned.error());
    }
    owned_batch.calls.push_back(std::move(owned).value());
  }
  return owned_batch;
}

[[nodiscard]] inline auto into_owned_tool_batch(tool_batch &&batch)
    -> wh::core::result<tool_batch> {
  tool_batch owned_batch{};
  owned_batch.calls.reserve(batch.calls.size());
  for (auto &call : batch.calls) {
    auto owned = into_owned_tool_call(std::move(call));
    if (owned.has_error()) {
      return wh::core::result<tool_batch>::failure(owned.error());
    }
    owned_batch.calls.push_back(std::move(owned).value());
  }
  return owned_batch;
}

[[nodiscard]] inline auto into_owned_tool_result(const tool_result &result)
    -> wh::core::result<tool_result> {
  auto value = wh::core::into_owned(result.value);
  if (value.has_error()) {
    return wh::core::result<tool_result>::failure(value.error());
  }
  return tool_result{
      .call_id = result.call_id,
      .tool_name = result.tool_name,
      .value = std::move(value).value(),
  };
}

[[nodiscard]] inline auto into_owned_tool_result(tool_result &&result)
    -> wh::core::result<tool_result> {
  auto value = wh::core::into_owned(std::move(result.value));
  if (value.has_error()) {
    return wh::core::result<tool_result>::failure(value.error());
  }
  return tool_result{
      .call_id = std::move(result.call_id),
      .tool_name = std::move(result.tool_name),
      .value = std::move(value).value(),
  };
}

[[nodiscard]] inline auto into_owned_tool_event(const tool_event &event)
    -> wh::core::result<tool_event> {
  auto value = wh::core::into_owned(event.value);
  if (value.has_error()) {
    return wh::core::result<tool_event>::failure(value.error());
  }
  return tool_event{
      .call_id = event.call_id,
      .tool_name = event.tool_name,
      .value = std::move(value).value(),
  };
}

[[nodiscard]] inline auto into_owned_tool_event(tool_event &&event)
    -> wh::core::result<tool_event> {
  auto value = wh::core::into_owned(std::move(event.value));
  if (value.has_error()) {
    return wh::core::result<tool_event>::failure(value.error());
  }
  return tool_event{
      .call_id = std::move(event.call_id),
      .tool_name = std::move(event.tool_name),
      .value = std::move(value).value(),
  };
}

} // namespace wh::compose::detail

namespace wh::core {

template <> struct any_owned_traits<wh::compose::tool_call> {
  [[nodiscard]] static auto into_owned(const wh::compose::tool_call &value)
      -> wh::core::result<wh::compose::tool_call> {
    return wh::compose::detail::into_owned_tool_call(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::tool_call &&value)
      -> wh::core::result<wh::compose::tool_call> {
    return wh::compose::detail::into_owned_tool_call(std::move(value));
  }
};

template <> struct any_owned_traits<wh::compose::tool_batch> {
  [[nodiscard]] static auto into_owned(const wh::compose::tool_batch &value)
      -> wh::core::result<wh::compose::tool_batch> {
    return wh::compose::detail::into_owned_tool_batch(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::tool_batch &&value)
      -> wh::core::result<wh::compose::tool_batch> {
    return wh::compose::detail::into_owned_tool_batch(std::move(value));
  }
};

template <> struct any_owned_traits<wh::compose::tool_result> {
  [[nodiscard]] static auto into_owned(const wh::compose::tool_result &value)
      -> wh::core::result<wh::compose::tool_result> {
    return wh::compose::detail::into_owned_tool_result(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::tool_result &&value)
      -> wh::core::result<wh::compose::tool_result> {
    return wh::compose::detail::into_owned_tool_result(std::move(value));
  }
};

template <> struct any_owned_traits<wh::compose::tool_event> {
  [[nodiscard]] static auto into_owned(const wh::compose::tool_event &value)
      -> wh::core::result<wh::compose::tool_event> {
    return wh::compose::detail::into_owned_tool_event(value);
  }

  [[nodiscard]] static auto into_owned(wh::compose::tool_event &&value)
      -> wh::core::result<wh::compose::tool_event> {
    return wh::compose::detail::into_owned_tool_event(std::move(value));
  }
};

} // namespace wh::core
