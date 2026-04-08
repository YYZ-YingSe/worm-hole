// Defines compose graph value, node contracts, edge data, and diagnostics.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/any.hpp"
#include "wh/core/component.hpp"
#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/schema/stream.hpp"

namespace wh::compose {

struct graph_call_options;
class graph_call_scope;

/// Type-erased node payload used across compose graph boundaries.
using graph_value = wh::core::any;
/// Type-erased graph stream reader used across compose graph boundaries.
using graph_stream_reader = wh::schema::stream::any_stream_reader<graph_value>;
/// Type-erased graph stream writer used across compose graph boundaries.
using graph_stream_writer = wh::schema::stream::any_stream_writer<graph_value>;
/// Type-erased async result boundary carrying one graph value.
using graph_value_sender =
    exec::any_receiver_ref<stdexec::completion_signatures<
        stdexec::set_value_t(wh::core::result<graph_value>),
        stdexec::set_stopped_t()>>::any_sender<>;
/// Logical contract for node input/output boundaries.
enum class node_contract : std::uint8_t {
  /// One scalar/object payload.
  value = 0U,
  /// One compose stream payload.
  stream,
};

/// Execution mode carried by one authored or compiled node.
/// `sync` nodes produce their result inline; `async` nodes hand execution to a
/// sender. Some node families let the user choose this; others set it
/// themselves.
enum class node_exec_mode : std::uint8_t {
  sync = 0U,
  async,
};

/// Records whether `exec_mode` came from user choice or built-in node rules.
enum class node_exec_origin : std::uint8_t {
  authored = 0U,
  lowered,
};

/// Returns a stable string label for one node contract.
[[nodiscard]] constexpr auto to_string(const node_contract contract) noexcept
    -> std::string_view {
  switch (contract) {
  case node_contract::value:
    return "value";
  case node_contract::stream:
    return "stream";
  }
  return "value";
}

/// Graph input/output contract declared at one graph boundary.
struct graph_boundary {
  /// Contract consumed by the reserved start boundary.
  node_contract input{node_contract::value};
  /// Contract produced by the reserved end boundary.
  node_contract output{node_contract::value};
};

/// Finite public/compiled node categories in compose.
enum class node_kind : std::uint8_t {
  /// Component-backed node.
  component = 0U,
  /// Lambda-backed node.
  lambda,
  /// Nested graph node.
  subgraph,
  /// Tools dispatch node.
  tools,
  /// Payload passthrough node.
  passthrough,
};

/// Returns the default exec-origin used by one built-in node family.
[[nodiscard]] constexpr auto default_exec_origin(const node_kind kind) noexcept
    -> node_exec_origin {
  switch (kind) {
  case node_kind::component:
  case node_kind::lambda:
  case node_kind::tools:
    return node_exec_origin::authored;
  case node_kind::subgraph:
  case node_kind::passthrough:
    return node_exec_origin::lowered;
  }
  return node_exec_origin::lowered;
}

/// Re-export of the core component family used by component-node binding rules.
using component_kind = wh::core::component_kind;

/// Named key/value map used by workflow field mapping and keyed payload IO.
using graph_value_map = std::unordered_map<std::string, graph_value,
                                           wh::core::transparent_string_hash,
                                           wh::core::transparent_string_equal>;

/// Authored edge-adapter families accepted by the public graph authoring API.
enum class edge_adapter_kind : std::uint8_t {
  /// No authored override; compile decides passthrough or builtin lowering.
  none = 0U,
  /// Run one user-provided adapter callback for non-default lowering.
  custom,
};

/// Runtime guardrails used by stream-collecting adapters.
struct edge_limits {
  /// Optional maximum chunk count allowed during `stream -> value` collection.
  std::size_t max_items{0U};
};

/// Custom edge adapter contract that produces one stream payload.
using edge_to_stream_adapter =
    wh::core::callback_function<wh::core::result<graph_stream_reader>(
        graph_value &&, const edge_limits &, wh::core::run_context &) const>;
/// Custom edge adapter contract that produces one value payload.
using edge_to_value_adapter = wh::core::callback_function<graph_value_sender(
    graph_stream_reader, const edge_limits &, wh::core::run_context &) const>;

/// Custom adapter hooks used when default contract bridging is insufficient.
struct custom_edge_adapter {
  /// Optional adapter that lowers one edge payload to stream contract.
  std::optional<edge_to_stream_adapter> to_stream{};
  /// Optional adapter that lowers one edge payload to value contract.
  std::optional<edge_to_value_adapter> to_value{};
};

/// Edge adapter declaration attached to one graph edge.
struct edge_adapter {
  /// Selected adapter family.
  edge_adapter_kind kind{edge_adapter_kind::none};
  /// Custom hooks used only when `kind == custom`.
  custom_edge_adapter custom{};
};

/// Extra edge configuration beyond pure topology.
struct edge_options {
  /// Disables control dependency tracking for this edge when true.
  bool no_control{false};
  /// Disables data dependency tracking for this edge when true.
  bool no_data{false};
  /// Optional contract adapter executed between source and target.
  edge_adapter adapter{};
  /// Optional runtime guardrails for default/custom adapters.
  edge_limits limits{};
};
/// Graph edge definition between two node keys.
struct graph_edge {
  /// Source node key.
  std::string from{};
  /// Target node key.
  std::string to{};
  /// Edge-local adapter and guardrail settings.
  edge_options options{};
};

/// Value-branch selector callback that returns destination node ids directly.
using graph_value_branch_selector_ids =
    wh::core::callback_function<wh::core::result<std::vector<std::uint32_t>>(
        const graph_value &, wh::core::run_context &, const graph_call_scope &)
                                    const>;

/// Stream-branch selector callback that returns destination node ids directly.
using graph_stream_branch_selector_ids =
    wh::core::callback_function<wh::core::result<std::vector<std::uint32_t>>(
        graph_stream_reader, wh::core::run_context &, const graph_call_scope &)
                                    const>;

/// Value-branch declaration from one source node to candidate end-nodes.
struct graph_value_branch {
  /// Branch source node key.
  std::string from{};
  /// Allowed branch destination node keys.
  std::vector<std::string> end_nodes{};
  /// Optional runtime selector that returns destination node ids directly.
  graph_value_branch_selector_ids selector_ids{nullptr};
};

/// Stream-branch declaration from one source node to candidate end-nodes.
struct graph_stream_branch {
  /// Branch source node key.
  std::string from{};
  /// Allowed branch destination node keys.
  std::vector<std::string> end_nodes{};
  /// Optional runtime selector that returns destination node ids directly.
  graph_stream_branch_selector_ids selector_ids{nullptr};
};

/// Compile/runtime diagnostic record emitted by graph operations.
struct graph_diagnostic {
  /// Structured error code.
  wh::core::error_code code{wh::core::errc::ok};
  /// Human-readable message containing contextual path/key details.
  std::string message{};
};

} // namespace wh::compose
