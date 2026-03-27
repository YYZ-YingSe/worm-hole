// Defines per-node metadata and validation flags used when adding graph nodes.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wh/compose/graph/compile_info.hpp"
#include "wh/core/callback.hpp"

namespace wh::compose {

/// One local callback registration stored on a node definition.
struct graph_node_callback_registration {
  /// Registration metadata such as stage filters and logical name.
  wh::core::callback_config config{};
  /// Stage-bound callbacks registered for this node scope.
  wh::core::stage_callbacks callbacks{};
};

/// Ordered node-local callback plan stored on one node definition.
using graph_node_callback_plan =
    std::vector<graph_node_callback_registration>;

/// Node-level observation defaults stored in graph definitions.
struct graph_node_observation {
  /// Enables callback emission for this node when runtime supports it.
  bool callbacks_enabled{true};
  /// True means invoke-time observation overrides may patch this node.
  bool allow_invoke_override{true};
  /// Default local callback registrations attached to this node.
  graph_node_callback_plan local_callbacks{};
};

/// Node-level metadata stored in graph definitions.
struct graph_add_node_options {
  /// Optional display name for diagnostics.
  std::string name{};
  /// Stable logical node type used by diagnostics and introspection.
  std::string type{};
  /// Logical input key consumed by keyed payload adapters.
  std::string input_key{};
  /// Logical output key produced by keyed payload adapters.
  std::string output_key{};
  /// Node-level observation defaults stored at registration time.
  graph_node_observation observation{};
  /// Optional display label emitted in introspection events.
  std::string label{};
  /// True means node may execute with no control predecessor.
  bool allow_no_control{false};
  /// True means node may execute with no data predecessor.
  bool allow_no_data{false};
  /// Optional node-level retry budget override (falls back to graph default).
  std::optional<std::size_t> retry_budget_override{};
  /// Optional node-level timeout override (`nullopt` falls back to graph default).
  std::optional<std::chrono::milliseconds> timeout_override{};
  /// Optional node-level retry window override used for policy composability checks.
  std::optional<std::chrono::milliseconds> retry_window_override{};
  /// Optional node-level parallel gate override (`>=1`).
  std::optional<std::size_t> max_parallel_override{};
  /// Optional node-level cache namespace override (empty disables cache on node).
  std::optional<std::string> cache_namespace_override{};
  /// Declares node-level expected state handlers (pre/post/stream hooks).
  graph_compile_state_handler_metadata state_handlers{};
  /// Optional subgraph compile snapshot forwarded to parent compile callback.
  std::optional<graph_compile_info> subgraph_compile_info{};
};

} // namespace wh::compose
