// Defines the internal graph storage and compiled-index types shared by graph
// build, compile, and runtime paths.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "wh/compose/graph/compile_info.hpp"
#include "wh/compose/graph/compile_options.hpp"
#include "wh/compose/graph/detail/bitset.hpp"
#include "wh/compose/graph/detail/runtime/input.hpp"
#include "wh/compose/graph/detail/runtime/invoke.hpp"
#include "wh/compose/graph/detail/runtime/pending_inputs.hpp"
#include "wh/compose/graph/edge_lowering.hpp"
#include "wh/compose/graph/error.hpp"
#include "wh/compose/graph/invoke_types.hpp"
#include "wh/compose/graph/restore_shape.hpp"
#include "wh/compose/graph/snapshot.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/types.hpp"

namespace wh::compose::detail {

struct graph_core {
  using dynamic_bitset = detail::dynamic_bitset;
  using dag_node_phase = detail::input_runtime::dag_node_phase;
  using dag_edge_status = detail::input_runtime::dag_edge_status;
  using dag_ready_state = detail::input_runtime::dag_ready_state;
  using pregel_ready_state = detail::input_runtime::pregel_ready_state;
  using input_lane = detail::input_runtime::input_lane;
  using dag_branch_state = detail::input_runtime::dag_branch_state;
  using reader_lane_state = detail::input_runtime::reader_lane_state;
  using reader_lowering = detail::input_runtime::reader_lowering;
  using runtime_io_storage = detail::input_runtime::runtime_io_storage;
  using dag_schedule_state = detail::input_runtime::dag_schedule_state;
  using pregel_node_inputs = detail::input_runtime::pregel_node_inputs;
  using pregel_delivery_store = detail::input_runtime::pregel_delivery_store;
  using resolved_input = detail::input_runtime::resolved_input;
  using value_input = detail::input_runtime::value_input;
  using value_batch = detail::input_runtime::value_batch;
  using edge_status = dag_edge_status;
  using ready_state = dag_ready_state;
  using branch_state = dag_branch_state;
  using io_storage = runtime_io_storage;
  using dag_schedule = dag_schedule_state;
  using invoke_stage = detail::invoke_runtime::stage;
  using attempt_id = detail::invoke_runtime::attempt_id;
  using attempt_slot = detail::invoke_runtime::attempt_slot;
  using state_step = detail::invoke_runtime::state_step;
  using ready_action_kind = detail::invoke_runtime::ready_action_kind;
  using ready_action = detail::invoke_runtime::ready_action;
  using pregel_action = detail::invoke_runtime::pregel_action;

  struct indexed_edge {
    std::uint32_t from{0U};
    std::uint32_t to{0U};
    node_contract source_output{node_contract::value};
    node_contract target_input{node_contract::value};
    edge_lowering_kind lowering_kind{edge_lowering_kind::none};
    bool no_control{false};
    bool no_data{false};
    edge_adapter adapter{};
    edge_limits limits{};
  };

  enum class edge_flow : std::uint8_t {
    value_value = 0U,
    value_reader,
    reader_value,
    reader_reader,
  };

  struct indexed_value_branch_definition {
    std::vector<std::uint32_t> end_nodes_sorted{};
    graph_value_branch_selector_ids selector_ids{nullptr};

    [[nodiscard]] auto contains(const std::uint32_t node_id) const noexcept
        -> bool {
      return std::binary_search(end_nodes_sorted.begin(), end_nodes_sorted.end(),
                                node_id);
    }
  };

  struct indexed_stream_branch_definition {
    std::vector<std::uint32_t> end_nodes_sorted{};
    graph_stream_branch_selector_ids selector_ids{nullptr};

    [[nodiscard]] auto contains(const std::uint32_t node_id) const noexcept
        -> bool {
      return std::binary_search(end_nodes_sorted.begin(), end_nodes_sorted.end(),
                                node_id);
    }
  };

  struct csr_edge_index {
    std::vector<std::uint32_t> offsets{};
    std::vector<std::uint32_t> edge_ids{};

    [[nodiscard]] auto edge_ids_for(const std::uint32_t node_id) const
        -> std::span<const std::uint32_t> {
      if (offsets.empty()) {
        return {};
      }
      const auto begin = offsets[node_id];
      const auto end = offsets[node_id + 1U];
      return std::span<const std::uint32_t>{edge_ids.data() + begin,
                                            end - begin};
    }
  };

  struct graph_index {
    static constexpr std::uint32_t no_branch_index =
        std::numeric_limits<std::uint32_t>::max();

    std::unordered_map<std::string, std::uint32_t,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        key_to_id{};
    std::vector<std::string> id_to_key{};
    std::vector<const compiled_node *> nodes_by_id{};
    std::vector<indexed_edge> indexed_edges{};
    csr_edge_index incoming_control_edges{};
    csr_edge_index incoming_data_edges{};
    csr_edge_index outgoing_data_edges{};
    csr_edge_index outgoing_control_edges{};
    std::vector<std::uint8_t> has_value_branch_by_source{};
    std::vector<std::uint32_t> value_branch_index_by_source{};
    std::vector<indexed_value_branch_definition> value_branch_defs{};
    std::vector<std::uint8_t> has_stream_branch_by_source{};
    std::vector<std::uint32_t> stream_branch_index_by_source{};
    std::vector<indexed_stream_branch_definition> stream_branch_defs{};
    std::vector<std::uint32_t> allow_no_control_ids{};
    std::vector<std::uint32_t> root_node_ids{};
    std::uint32_t start_id{0U};
    std::uint32_t end_id{0U};

    [[nodiscard]] auto incoming_control(const std::uint32_t node_id) const
        -> std::span<const std::uint32_t> {
      return incoming_control_edges.edge_ids_for(node_id);
    }

    [[nodiscard]] auto incoming_data(const std::uint32_t node_id) const
        -> std::span<const std::uint32_t> {
      return incoming_data_edges.edge_ids_for(node_id);
    }

    [[nodiscard]] auto outgoing_data(const std::uint32_t node_id) const
        -> std::span<const std::uint32_t> {
      return outgoing_data_edges.edge_ids_for(node_id);
    }

    [[nodiscard]] auto outgoing_control(const std::uint32_t node_id) const
        -> std::span<const std::uint32_t> {
      return outgoing_control_edges.edge_ids_for(node_id);
    }

    [[nodiscard]] auto value_branch_for_source(
        const std::uint32_t source_id) const
        -> const indexed_value_branch_definition * {
      if (source_id >= has_value_branch_by_source.size() ||
          has_value_branch_by_source[source_id] == 0U) {
        return nullptr;
      }
      const auto branch_index = value_branch_index_by_source[source_id];
      if (branch_index == no_branch_index ||
          branch_index >= value_branch_defs.size()) {
        return nullptr;
      }
      return &value_branch_defs[branch_index];
    }

    [[nodiscard]] auto stream_branch_for_source(
        const std::uint32_t source_id) const
        -> const indexed_stream_branch_definition * {
      if (source_id >= has_stream_branch_by_source.size() ||
          has_stream_branch_by_source[source_id] == 0U) {
        return nullptr;
      }
      const auto branch_index = stream_branch_index_by_source[source_id];
      if (branch_index == no_branch_index ||
          branch_index >= stream_branch_defs.size()) {
        return nullptr;
      }
      return &stream_branch_defs[branch_index];
    }
  };

  struct output_plan {
    std::vector<std::uint32_t> reader_edges{};
  };

  struct input_plan {
    std::vector<std::uint32_t> value_edges{};
    std::vector<std::uint32_t> reader_edges{};
  };

  struct graph_plan {
    std::vector<edge_flow> edges{};
    std::vector<output_plan> outputs{};
    std::vector<input_plan> inputs{};
  };

  struct compiled_execution_index {
    graph_index index{};
    graph_plan plan{};
  };

  struct control_graph_index {
    std::unordered_map<std::string, std::uint32_t,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        key_to_id{};
    std::vector<std::uint32_t> control_out_offsets{};
    std::vector<std::uint32_t> control_out_nodes{};
    std::vector<std::size_t> indegree{};
    std::uint32_t start_id{0U};
    std::uint32_t end_id{0U};

    [[nodiscard]] auto out_neighbors(const std::uint32_t node_id) const
        -> std::span<const std::uint32_t> {
      const auto begin = control_out_offsets[node_id];
      const auto end = control_out_offsets[node_id + 1U];
      return std::span<const std::uint32_t>{control_out_nodes.data() + begin,
                                            end - begin};
    }
  };

  struct value_branch_definition {
    std::vector<std::string> end_nodes{};
    graph_value_branch_selector_ids selector_ids{nullptr};
  };

  struct stream_branch_definition {
    std::vector<std::string> end_nodes{};
    graph_stream_branch_selector_ids selector_ids{nullptr};
  };

  graph_compile_options options_{};
  std::unordered_map<std::string, authored_node,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      nodes_{};
  std::vector<compiled_node> compiled_nodes_{};
  std::vector<std::string> node_insertion_order_{};
  std::unordered_map<std::string, std::uint32_t,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      node_id_index_{};
  std::vector<graph_edge> edges_{};
  std::unordered_map<std::string, value_branch_definition,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      value_branches_{};
  std::unordered_map<std::string, stream_branch_definition,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      stream_branches_{};
  std::vector<graph_diagnostic> diagnostics_{};
  std::vector<std::string> compile_order_{};
  compiled_execution_index compiled_execution_index_{};
  mutable std::optional<graph_snapshot> snapshot_cache_{};
  mutable std::optional<std::once_flag> snapshot_once_{std::in_place};
  graph_restore_shape restore_shape_{};
  input_gate boundary_input_gate_{input_gate::open()};
  output_gate boundary_output_gate_{output_gate::dynamic()};
  bool compiled_{false};
  std::optional<wh::core::error_code> first_error_{};

  auto reset_snapshot_state() -> void {
    snapshot_cache_.reset();
    snapshot_once_.emplace();
  }

  auto copy_from(const graph_core &other) -> void {
    options_ = other.options_;
    nodes_ = other.nodes_;
    compiled_nodes_ = other.compiled_nodes_;
    node_insertion_order_ = other.node_insertion_order_;
    node_id_index_ = other.node_id_index_;
    edges_ = other.edges_;
    value_branches_ = other.value_branches_;
    stream_branches_ = other.stream_branches_;
    diagnostics_ = other.diagnostics_;
    compile_order_ = other.compile_order_;
    compiled_execution_index_ = other.compiled_execution_index_;
    snapshot_cache_ = other.snapshot_cache_;
    snapshot_once_.emplace();
    restore_shape_ = other.restore_shape_;
    boundary_input_gate_ = other.boundary_input_gate_;
    boundary_output_gate_ = other.boundary_output_gate_;
    compiled_ = other.compiled_;
    first_error_ = other.first_error_;
  }

  auto move_from(graph_core &other) -> void {
    options_ = std::move(other.options_);
    nodes_ = std::move(other.nodes_);
    compiled_nodes_ = std::move(other.compiled_nodes_);
    node_insertion_order_ = std::move(other.node_insertion_order_);
    node_id_index_ = std::move(other.node_id_index_);
    edges_ = std::move(other.edges_);
    value_branches_ = std::move(other.value_branches_);
    stream_branches_ = std::move(other.stream_branches_);
    diagnostics_ = std::move(other.diagnostics_);
    compile_order_ = std::move(other.compile_order_);
    compiled_execution_index_ = std::move(other.compiled_execution_index_);
    snapshot_cache_ = std::move(other.snapshot_cache_);
    snapshot_once_.emplace();
    restore_shape_ = std::move(other.restore_shape_);
    boundary_input_gate_ = other.boundary_input_gate_;
    boundary_output_gate_ = other.boundary_output_gate_;
    compiled_ = other.compiled_;
    first_error_ = std::move(other.first_error_);
    other.reset_snapshot_state();
  }

  auto clear_cold_authoring_state() -> void {
    nodes_.clear();
    nodes_.rehash(0U);
    edges_.clear();
    edges_.shrink_to_fit();
    value_branches_.clear();
    value_branches_.rehash(0U);
    stream_branches_.clear();
    stream_branches_.rehash(0U);
    node_insertion_order_.clear();
    node_insertion_order_.shrink_to_fit();
    node_id_index_.clear();
    node_id_index_.rehash(0U);
    compile_order_.clear();
    compile_order_.shrink_to_fit();
  }
};

} // namespace wh::compose::detail
