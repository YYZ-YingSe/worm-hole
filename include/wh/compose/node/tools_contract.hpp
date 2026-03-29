// Defines the public tools-node contract surface.
#pragma once

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
using tools_stream_sender =
    exec::any_receiver_ref<stdexec::completion_signatures<stdexec::set_value_t(
        wh::core::result<graph_stream_reader>),
                                   stdexec::set_stopped_t()>>::any_sender<>;

/// Tool invoke endpoint contract.
using tool_invoke = wh::core::callback_function<
    wh::core::result<graph_value>(const tool_call &, wh::tool::call_scope) const>;

/// Tool stream endpoint contract.
using tool_stream = wh::core::callback_function<
    wh::core::result<graph_stream_reader>(const tool_call &, wh::tool::call_scope) const>;

/// Async tool invoke endpoint contract.
using tool_async_invoke = wh::core::callback_function<
    tools_invoke_sender(tool_call, wh::tool::call_scope) const>;

/// Async tool stream endpoint contract.
using tool_async_stream = wh::core::callback_function<
    tools_stream_sender(tool_call, wh::tool::call_scope) const>;

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
using tool_registry =
    std::unordered_map<std::string, tool_entry,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>;

/// Before/after hooks shared by invoke and stream paths.
struct tool_middleware {
  /// Optional pre-call hook that may rewrite the concrete tool call.
  wh::core::callback_function<wh::core::result<void>(
      tool_call &, const wh::tool::call_scope &) const>
      before{nullptr};
  /// Optional post-call hook that may rewrite one emitted payload.
  wh::core::callback_function<wh::core::result<void>(
      const tool_call &, graph_value &, const wh::tool::call_scope &) const>
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
struct tools_call_options {
  /// Optional override registry used for this invoke only.
  std::optional<std::reference_wrapper<const tool_registry>> registry{};
  /// Optional sequential override.
  std::optional<bool> sequential{};
  /// Optional caller-owned rerun state shared with this invoke.
  tools_rerun *rerun{nullptr};
};

} // namespace wh::compose
