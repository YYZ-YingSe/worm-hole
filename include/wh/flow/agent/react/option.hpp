// Defines flow-level ReAct authoring options without introducing a second
// execution runtime.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/agent/react.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/message.hpp"

namespace wh::flow::agent::react {

/// First-stage message rewrite hook applied before the mutable modifier stage.
using message_rewriter = wh::core::callback_function<
    wh::core::result<std::vector<wh::schema::message>>(
        std::span<const wh::schema::message>) const>;

/// Second-stage message modifier hook applied to a copied message vector.
using message_modifier = wh::core::callback_function<
    wh::core::result<void>(std::vector<wh::schema::message> &) const>;

/// Tool-call detector used when callers inspect a streaming model response.
using stream_tool_call_checker = wh::core::callback_function<
    wh::core::result<bool>(wh::model::chat_message_stream_reader &) const>;

/// Per-run overrides applied on top of the authored ReAct flow.
struct react_run_options {
  /// Tool names that should behave as return-direct for this run.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      return_direct_tool_names{};
};

/// Frozen authoring options for one ReAct flow shell.
struct react_options {
  /// Stable exported graph name.
  std::string graph_name{"react_graph"};
  /// Stable exported model-node display name.
  std::string model_node_name{"react_model"};
  /// Stable exported tools-node display name.
  std::string tools_node_name{"react_tools"};
  /// Maximum model iterations before failure.
  std::size_t max_iterations{20U};
  /// True forwards intermediate assistant/tool messages into the reader path.
  bool emit_internal_messages{true};
  /// Optional output slot forwarded to the wrapped ADK ReAct agent.
  std::string output_key{};
  /// Output materialization mode forwarded to the wrapped ADK ReAct agent.
  wh::agent::react_output_mode output_mode{wh::agent::react_output_mode::value};
  /// First-stage immutable rewrite hook.
  message_rewriter rewrite_messages{nullptr};
  /// Second-stage mutable rewrite hook.
  message_modifier modify_messages{nullptr};
  /// Optional caller-provided stream tool-call detector.
  stream_tool_call_checker detect_tool_calls{nullptr};
};

/// Default stream tool-call detector based on the first non-empty chunk.
[[nodiscard]] inline auto default_stream_tool_call_checker(
    wh::model::chat_message_stream_reader &reader) -> wh::core::result<bool> {
  while (true) {
    auto next = reader.read();
    if (next.has_error()) {
      return wh::core::result<bool>::failure(next.error());
    }
    if (next.value().error.failed()) {
      return wh::core::result<bool>::failure(next.value().error);
    }
    if (next.value().eof) {
      return false;
    }
    if (!next.value().value.has_value()) {
      continue;
    }
    for (const auto &part : next.value().value->parts) {
      if (std::holds_alternative<wh::schema::tool_call_part>(part)) {
        return true;
      }
      if (const auto *text = std::get_if<wh::schema::text_part>(&part);
          text != nullptr && !text->text.empty()) {
        return false;
      }
    }
  }
}

} // namespace wh::flow::agent::react
