// Defines compile-time graph introspection snapshots passed to compile callbacks.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "wh/compose/graph/policy.hpp"
#include "wh/compose/types.hpp"

namespace wh::compose {

/// Node-level state-handler metadata snapshot exported in compile info.
struct graph_compile_state_handler_metadata {
  /// True when value pre-handler is expected for this node.
  bool pre{false};
  /// True when value post-handler is expected for this node.
  bool post{false};
  /// True when stream pre-handler is expected for this node.
  bool stream_pre{false};
  /// True when stream post-handler is expected for this node.
  bool stream_post{false};

  /// True when at least one state handler is expected.
  [[nodiscard]] constexpr auto any() const noexcept -> bool {
    return pre || post || stream_pre || stream_post;
  }
};

/// Node-level observation defaults exported in compile callbacks/introspection.
struct graph_compile_node_observation_info {
  /// Enables callback emission for this node when runtime supports it.
  bool callbacks_enabled{true};
  /// True means invoke-time observation overrides may patch this node.
  bool allow_invoke_override{true};
  /// Number of default local callback registrations carried by the node.
  std::size_t local_callback_count{0U};
};

/// Node-options snapshot exported in compile callbacks/introspection.
struct graph_compile_node_options_info {
  /// Optional display name for diagnostics.
  std::string name{};
  /// Stable logical node type used by diagnostics and introspection.
  std::string type{};
  /// Logical input key consumed by keyed payload adapters.
  std::string input_key{};
  /// Logical output key produced by keyed payload adapters.
  std::string output_key{};
  /// Node-level observation defaults resolved at registration time.
  graph_compile_node_observation_info observation{};
  /// Optional display label emitted in introspection events.
  std::string label{};
  /// Missing-rerun-input autofill contract (`value` or `stream`).
  std::uint8_t input_contract{0U};
  /// True means node may execute with no control predecessor.
  bool allow_no_control{false};
  /// True means node may execute with no data predecessor.
  bool allow_no_data{false};
  /// Optional node-level retry budget override.
  std::optional<std::size_t> retry_budget_override{};
  /// Optional node-level timeout override (`nullopt` falls back to graph default).
  std::optional<std::chrono::milliseconds> timeout_override{};
  /// Optional node-level retry window override (`timeout` must be smaller).
  std::optional<std::chrono::milliseconds> retry_window_override{};
  /// Optional node-level parallel gate override (`>=1`).
  std::optional<std::size_t> max_parallel_override{};
  /// Node-level state-handler metadata expectations.
  graph_compile_state_handler_metadata state_handlers{};
};

/// Node-level keyed IO mapping snapshot exported in compile callbacks.
struct graph_compile_field_mapping_info {
  /// Logical input key consumed by keyed payload adapters.
  std::string input_key{};
  /// Logical output key produced by keyed payload adapters.
  std::string output_key{};
};

/// One node entry exported in compile callback graph info.
struct graph_compile_node_info {
  /// Stable node key.
  std::string key{};
  /// Stable runtime node id.
  std::uint32_t node_id{0U};
  /// True when node has runtime sender instance.
  bool has_sender{false};
  /// True when node exports subgraph compile snapshot.
  bool has_subgraph{false};
  /// Node-level keyed IO mapping snapshot.
  graph_compile_field_mapping_info field_mapping{};
  /// Node-level compile metadata snapshot.
  graph_compile_node_options_info options{};
};

/// One branch entry exported in compile callback graph info.
struct graph_compile_branch_info {
  /// Branch source node key.
  std::string from{};
  /// Allowed branch destination keys.
  std::vector<std::string> end_nodes{};
};

/// Immutable graph compile snapshot for top-level/subgraph callback delivery.
struct graph_compile_info {
  /// Graph name.
  std::string name{"graph"};
  /// Runtime mode (`dag`/`pregel`) used by this graph.
  graph_runtime_mode mode{graph_runtime_mode::dag};
  /// Eager dispatch flag controlling immediate/deferred dependent scheduling.
  bool eager{true};
  /// Compile-time max-step budget.
  std::size_t max_steps{1024U};
  /// Graph-level trigger policy.
  graph_trigger_mode trigger_mode{graph_trigger_mode::any_predecessor};
  /// Graph-level fan-in policy.
  graph_fan_in_policy fan_in_policy{graph_fan_in_policy::allow_partial};
  /// Graph-level default retry budget.
  std::size_t retry_budget{0U};
  /// Graph-level default node timeout budget (`nullopt` means no timeout).
  std::optional<std::chrono::milliseconds> node_timeout{};
  /// Graph-level global parallel activation gate (`>=1`).
  std::size_t max_parallel_nodes{1U};
  /// Graph-level default per-node parallel gate (`>=1`).
  std::size_t max_parallel_per_node{1U};
  /// True means graph enables local state generation features.
  bool state_generator_enabled{true};
  /// Deterministic compile order (topological/SCC-condensed order).
  std::vector<std::string> compile_order{};
  /// Runtime node-key to node-id mapping snapshot.
  std::unordered_map<std::string, std::uint32_t> node_key_to_id{};
  /// Node snapshots keyed by insertion/compile registration.
  std::vector<graph_compile_node_info> nodes{};
  /// Edge snapshots (`from/to/no_control/no_data`).
  std::vector<graph_edge> edges{};
  /// Control-edge snapshots (`no_control == false`).
  std::vector<graph_edge> control_edges{};
  /// Data-edge snapshots (`no_data == false`).
  std::vector<graph_edge> data_edges{};
  /// Branch snapshots (`from/end_nodes`).
  std::vector<graph_compile_branch_info> branches{};
  /// Subgraph compile snapshots keyed by parent node key.
  std::unordered_map<std::string, graph_compile_info> subgraphs{};
};

} // namespace wh::compose
