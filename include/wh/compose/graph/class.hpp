// Defines the public compose graph class declaration.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <exec/create.hpp>
#include <exec/env.hpp>
#include <exec/timed_thread_scheduler.hpp>
#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/node.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/compose/graph/error.hpp"
#include "wh/compose/graph/invoke_types.hpp"
#include "wh/compose/graph/detail/bitset.hpp"
#include "wh/compose/graph/detail/runtime/rerun.hpp"
#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/compile_options.hpp"
#include "wh/compose/graph/detail/runtime/checkpoint.hpp"
#include "wh/compose/graph/detail/runtime/interrupt.hpp"
#include "wh/compose/graph/detail/runtime/input.hpp"
#include "wh/compose/graph/like.hpp"
#include "wh/compose/graph/detail/runtime/invoke.hpp"
#include "wh/compose/graph/detail/runtime/process.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/graph/detail/runtime/handlers.hpp"
#include "wh/compose/graph/detail/runtime/stream.hpp"
#include "wh/compose/graph/restore_shape.hpp"
#include "wh/compose/graph/snapshot.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/compose/types.hpp"
#include "wh/compose/reduce/values_merge.hpp"
#include "wh/core/address.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

class graph;

namespace detail {

using string_hash = wh::core::transparent_string_hash;
using string_equal = wh::core::transparent_string_equal;

namespace invoke_runtime {
class run_state;
}

[[nodiscard]] auto start_bound_graph(
    const wh::compose::graph &graph, wh::core::run_context &context,
    graph_value &input, const graph_call_options *call_options,
    const node_path *path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
    const invoke_runtime::run_state *parent_state = nullptr,
    const graph_runtime_services *services = nullptr,
    graph_invoke_controls controls = {})
    -> graph_sender;

} // namespace detail

/// Mutable graph definition that compiles into a stable executable topology.
class graph {
  friend class detail::invoke_runtime::run_state;

  template <typename receiver_t>
  class invoke_operation;

  template <typename request_t>
  class invoke_sender;

  using invoke_completion_signatures =
      stdexec::completion_signatures<
          stdexec::set_value_t(wh::core::result<graph_invoke_result>)>;

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
  [[nodiscard]] auto make_invoke_sender(request_t &&request,
                                        wh::core::run_context &context) const
      -> invoke_sender<graph_invoke_request>;

public:
  graph();

  explicit graph(graph_boundary boundary);

  explicit graph(const graph_compile_options &options);

  explicit graph(graph_compile_options &&options);

  graph(graph_boundary boundary, const graph_compile_options &options);

  graph(graph_boundary boundary, graph_compile_options &&options);

  graph(const graph &other);

  graph(graph &&other) noexcept;

  auto operator=(const graph &other) -> graph &;

  auto operator=(graph &&other) noexcept -> graph &;

  /// Returns immutable graph compile options.
  [[nodiscard]] auto options() const noexcept -> const graph_compile_options &;

  /// Returns declared graph boundary contract.
  [[nodiscard]] auto boundary() const noexcept -> const graph_boundary &;

  /// Returns compile options snapshot used by diff and restore validation.
  [[nodiscard]] auto compile_options_snapshot() const -> graph_compile_options;

  /// Returns compile-stable graph snapshot used by diff and restore validation.
  [[nodiscard]] auto snapshot() const -> graph_snapshot;

  /// Returns compile-stable graph snapshot by immutable reference.
  [[nodiscard]] auto snapshot_view() const -> const graph_snapshot &;

  /// Returns compile-stable restore shape used by checkpoint validation.
  [[nodiscard]] auto restore_shape() const noexcept
      -> const graph_restore_shape &;

  /// Returns true after compile succeeds.
  [[nodiscard]] auto compiled() const noexcept -> bool;

  /// Returns diagnostics collected during graph build/compile/invoke.
  [[nodiscard]] auto diagnostics() const noexcept
      -> const std::vector<graph_diagnostic> &;

  /// Returns compile order captured by latest successful compile.
  [[nodiscard]] auto compile_order() const noexcept
      -> const std::vector<std::string> &;

  /// Returns stable runtime node id for `key` based on insertion order.
  [[nodiscard]] auto node_id(const std::string_view key) const
      -> wh::core::result<std::uint32_t>;

  /// Returns one compiled runtime node after `compile()` succeeds.
  [[nodiscard]] auto compiled_node_by_key(const std::string_view key) const
      -> wh::core::result<std::reference_wrapper<const compiled_node>>;

  /// Registers a component node.
  auto add_component(const component_node &node) -> wh::core::result<void>;

  /// Registers a component node.
  auto add_component(component_node &&node) -> wh::core::result<void>;

  /// Registers a lambda node.
  auto add_lambda(const lambda_node &node) -> wh::core::result<void>;

  /// Registers a lambda node.
  auto add_lambda(lambda_node &&node) -> wh::core::result<void>;

  /// Registers a subgraph node.
  auto add_subgraph(const subgraph_node &node) -> wh::core::result<void>;

  /// Registers a subgraph node.
  auto add_subgraph(subgraph_node &&node) -> wh::core::result<void>;

  /// Registers a tools node.
  auto add_tools(const tools_node &node) -> wh::core::result<void>;

  /// Registers a tools node.
  auto add_tools(tools_node &&node) -> wh::core::result<void>;

  /// Registers a passthrough node.
  auto add_passthrough(const passthrough_node &node)
      -> wh::core::result<void>;

  /// Registers a passthrough node.
  auto add_passthrough(passthrough_node &&node) -> wh::core::result<void>;

  template <node_contract From = node_contract::value,
            node_contract To = node_contract::value,
            node_exec_mode Exec = node_exec_mode::sync, typename key_t,
            typename lambda_t,
            typename options_t = graph_add_node_options>
  /// Registers a lambda node.
  auto add_lambda(key_t &&key, lambda_t &&lambda, options_t &&options = {})
      -> wh::core::result<void>;

  template <component_kind Kind, node_contract From, node_contract To,
            node_exec_mode Exec = node_exec_mode::sync,
            typename key_t, typename component_t,
            typename options_t = graph_add_node_options>
    requires (Kind != component_kind::custom)
  /// Registers a component node.
  auto add_component(key_t &&key, component_t &&component,
                     options_t &&options = {}) -> wh::core::result<void>;

  template <typename key_t, typename graph_t,
            typename options_t = graph_add_node_options>
    requires graph_viewable<std::remove_cvref_t<graph_t>>
  /// Registers a subgraph node.
  auto add_subgraph(key_t &&key, graph_t &&subgraph,
                    options_t &&options = {}) -> wh::core::result<void>;

  template <node_contract From = node_contract::value,
            node_contract To = node_contract::value,
            node_exec_mode Exec = node_exec_mode::sync, typename key_t,
            typename registry_t,
            typename options_t = graph_add_node_options,
            typename tool_options_t = tools_options>
  /// Registers a tools node.
  auto add_tools(key_t &&key, registry_t &&registry, options_t &&options = {},
                 tool_options_t &&tool_options = {}) -> wh::core::result<void>;

  template <node_contract Contract = node_contract::value, typename key_t>
  /// Registers a passthrough node.
  auto add_passthrough(key_t &&key) -> wh::core::result<void>;

  /// Registers one edge between two existing nodes.
  auto add_edge(const graph_edge &edge) -> wh::core::result<void>;

  /// Registers one edge between two existing nodes.
  auto add_edge(graph_edge &&edge) -> wh::core::result<void>;

  template <typename from_t, typename to_t>
    requires std::constructible_from<std::string, from_t &&> &&
             std::constructible_from<std::string, to_t &&>
  /// Registers one edge from source key to target key with edge options.
  auto add_edge(from_t &&from, to_t &&to, edge_options options = {})
      -> wh::core::result<void>;

  template <typename to_t>
    requires std::constructible_from<std::string, to_t &&>
  /// Registers one edge from the reserved graph entry node.
  auto add_entry_edge(to_t &&to, edge_options options = {})
      -> wh::core::result<void>;

  template <typename from_t>
    requires std::constructible_from<std::string, from_t &&>
  /// Registers one edge into the reserved graph exit node.
  auto add_exit_edge(from_t &&from, edge_options options = {})
      -> wh::core::result<void>;

  /// Registers one branch declaration by expanding to multiple edges.
  auto add_branch(const graph_branch &branch) -> wh::core::result<void>;

  /// Registers one branch declaration by expanding to multiple edges.
  auto add_branch(graph_branch &&branch) -> wh::core::result<void>;

  /// Runs compile validations and freezes graph structure.
  auto compile() -> wh::core::result<void>;

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
  /// Invokes graph with typed controls/services and returns one structured result.
  [[nodiscard]] auto invoke(wh::core::run_context &context,
                            request_t &&request) const -> auto;

private:
  friend class detail::invoke_runtime::run_state;
  friend auto detail::start_bound_graph(
      const graph &graph, wh::core::run_context &context,
      graph_value &input, const graph_call_options *call_options,
      const node_path *path_prefix, graph_process_state *parent_process_state,
      detail::runtime_state::invoke_outputs *nested_outputs,
      const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
      const detail::invoke_runtime::run_state *parent_state,
      const graph_runtime_services *services, graph_invoke_controls controls)
      -> graph_sender;
  friend auto detail::start_nested_graph(const graph &graph,
                                         wh::core::run_context &context,
                                         graph_value &input,
                                         const node_runtime &runtime)
      -> graph_sender;

  [[nodiscard]] static auto next_invoke_run_id() noexcept -> std::uint64_t;

  using dynamic_bitset = detail::dynamic_bitset;
  using node_state = detail::input_runtime::node_state;
  using edge_status = detail::input_runtime::edge_status;
  using ready_state = detail::input_runtime::ready_state;
  using input_lane = detail::input_runtime::input_lane;
  using branch_state = detail::input_runtime::branch_state;
  using reader_lane_state = detail::input_runtime::reader_lane_state;
  using reader_lowering = detail::input_runtime::reader_lowering;
  using scratch_buffer = detail::input_runtime::scratch_buffer;
  using resolved_input = detail::input_runtime::resolved_input;
  using value_input = detail::input_runtime::value_input;
  using value_batch = detail::input_runtime::value_batch;
  using invoke_stage = detail::invoke_runtime::stage;
  using node_frame = detail::invoke_runtime::node_frame;
  using state_step = detail::invoke_runtime::state_step;
  using ready_action_kind = detail::invoke_runtime::ready_action_kind;
  using ready_action = detail::invoke_runtime::ready_action;
  using pregel_action = detail::invoke_runtime::pregel_action;

  [[nodiscard]] auto collect_completed_nodes(
      const std::vector<node_state> &node_states) const
      -> std::vector<std::string>;

  auto publish_last_completed_nodes(
      detail::runtime_state::invoke_outputs &outputs,
      const std::vector<node_state> &node_states) const -> void;

  /// Internal edge representation with compact integer node ids.
  struct indexed_edge {
    std::uint32_t from{0U};
    std::uint32_t to{0U};
    node_contract source_output{node_contract::value};
    node_contract target_input{node_contract::value};
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

  /// Branch metadata compiled to node ids for hot-path lookups.
  struct indexed_branch_definition {
    std::vector<std::uint32_t> end_nodes_sorted{};
    graph_branch_selector_ids selector_ids{nullptr};

    [[nodiscard]] auto contains(const std::uint32_t node_id) const noexcept -> bool {
      return std::binary_search(end_nodes_sorted.begin(), end_nodes_sorted.end(),
                                node_id);
    }
  };

  /// CSR adjacency table keyed by node id.
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
      return std::span<const std::uint32_t>{edge_ids.data() + begin, end - begin};
    }
  };

  struct graph_index {
    static constexpr std::uint32_t no_branch_index =
        std::numeric_limits<std::uint32_t>::max();

    std::unordered_map<std::string, std::uint32_t, detail::string_hash,
                       detail::string_equal>
        key_to_id{};
    std::vector<std::string> id_to_key{};
    std::vector<const compiled_node *> nodes_by_id{};
    std::vector<indexed_edge> indexed_edges{};
    csr_edge_index incoming_control_edges{};
    csr_edge_index incoming_data_edges{};
    csr_edge_index outgoing_data_edges{};
    csr_edge_index outgoing_control_edges{};
    std::vector<std::uint8_t> has_branch_by_source{};
    std::vector<std::uint32_t> branch_index_by_source{};
    std::vector<indexed_branch_definition> branch_defs{};
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

    [[nodiscard]] auto branch_for_source(const std::uint32_t source_id) const
        -> const indexed_branch_definition * {
      if (source_id >= has_branch_by_source.size() ||
          has_branch_by_source[source_id] == 0U) {
        return nullptr;
      }
      const auto branch_index = branch_index_by_source[source_id];
      if (branch_index == no_branch_index || branch_index >= branch_defs.size()) {
        return nullptr;
      }
      return &branch_defs[branch_index];
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

  /// Immutable compiled execution index built by compile() and shared by invoke().
  struct compiled_execution_index {
    graph_index index{};
    graph_plan plan{};
  };

  /// Compile-time control graph index used by validate/reachability/sort.
  struct control_graph_index {
    std::unordered_map<std::string, std::uint32_t, detail::string_hash,
                       detail::string_equal>
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

  /// Immutable branch definition stored by source node key.
  struct branch_definition {
    /// Allowed destination keys.
    std::vector<std::string> end_nodes{};
    /// Optional selector callback that returns destination node ids.
    graph_branch_selector_ids selector_ids{nullptr};
  };

  template <typename node_t>
  auto add_node_impl(node_t &&node) -> wh::core::result<void>;

  [[nodiscard]] static constexpr auto
  default_edge_adapter_kind(const node_contract source_output,
                            const node_contract target_input) noexcept
      -> edge_adapter_kind;

  [[nodiscard]] auto resolve_edge_adapter(const graph_edge &edge) const
      -> wh::core::result<edge_adapter>;

  template <typename edge_t>
  auto add_edge_impl(edge_t &&edge) -> wh::core::result<void>;

  template <typename branch_t>
  auto add_branch_impl(branch_t &&branch) -> wh::core::result<void>;

  auto validate_edges() -> wh::core::result<void>;

  auto validate_node_output_keys() -> wh::core::result<void>;

  auto validate_node_policy_overrides() -> wh::core::result<void>;

  [[nodiscard]] static auto to_compile_node_options_info(
      const graph_add_node_options &options, const node_contract input_contract)
      -> graph_compile_node_options_info;

  [[nodiscard]] auto build_compile_info_snapshot() const -> graph_compile_info;

  [[nodiscard]] auto build_snapshot() const -> graph_snapshot;

  [[nodiscard]] auto build_restore_shape() const -> graph_restore_shape;

  auto rebind_compiled_execution_index_nodes() noexcept -> void;

  [[nodiscard]] static auto
  make_csr_offsets(const std::vector<std::uint32_t> &counts)
      -> std::vector<std::uint32_t>;

  [[nodiscard]] static auto compile_authored(const authored_node &node)
      -> compiled_node;

  auto build_compiled_execution_index() -> wh::core::result<void>;

  auto validate_contracts() -> wh::core::result<void>;

  [[nodiscard]] auto validate_call_scope_for_runtime(
      const graph_call_scope &call_scope) const -> wh::core::result<void>;

  [[nodiscard]] auto make_node_designation_path(const std::uint32_t node_id) const
      -> node_path;

  [[nodiscard]] auto make_runtime_node_path(const node_path &prefix,
                                            const std::uint32_t node_id) const
      -> node_path;

  auto publish_node_run_error(detail::runtime_state::invoke_outputs &outputs,
                              const node_path &runtime_path,
                              const std::uint32_t node_id,
                              const wh::core::error_code code,
                              const std::string_view message) const -> void;

  auto publish_graph_run_error(
      detail::runtime_state::invoke_outputs &outputs,
      const std::optional<node_path> &runtime_path,
      const std::string_view node_key, const compose_error_phase phase,
      const wh::core::error_code code, const std::string_view message,
      const std::optional<wh::core::error_code> raw_error = std::nullopt) const
      -> void;

  auto publish_stream_read_error(detail::runtime_state::invoke_outputs &outputs,
                                 node_path runtime_path,
                                 const std::string_view node_key,
                                 const wh::core::error_code code,
                                 const std::string_view message) const -> void;

  [[nodiscard]] static constexpr auto
  should_wrap_as_node_run_error(const wh::core::error_code code) noexcept -> bool;

  [[nodiscard]] auto make_node_execution_address(const node_path &runtime_path) const
      -> wh::core::address;

  [[nodiscard]] static auto
  has_descendant_designation_target(const graph_call_scope &call_options,
                                    const node_path &path) -> bool;

  [[nodiscard]] static auto has_active_designation(
      const graph_call_scope &options) noexcept -> bool;

  [[nodiscard]] auto is_node_designated(
      const std::uint32_t node_id, const graph_call_scope &call_options) const
      -> bool;

  auto emit_debug_stream_event(
      wh::core::run_context &context,
      detail::runtime_state::invoke_outputs &outputs,
      const graph_call_scope &options,
      const graph_debug_stream_event::decision_kind decision,
      const std::uint32_t node_id, const node_path &runtime_path,
      const std::size_t step) const -> void;

  [[nodiscard]] auto make_stream_scope(const std::string_view node_key) const
      -> graph_event_scope;

  [[nodiscard]] auto make_stream_scope(const std::string_view node_key,
                                       const node_path &runtime_path) const
      -> graph_event_scope;

  auto apply_state_phase(wh::core::run_context &context,
                         const graph_node_state_handlers *handlers,
                         detail::state_runtime::state_phase phase,
                         const std::string_view node_key,
                         const graph_state_cause &cause,
                         graph_process_state &process_state,
                         graph_value &payload, const node_path &runtime_path,
                         detail::runtime_state::invoke_outputs &outputs) const
      -> wh::core::result<void>;

  [[nodiscard]] auto apply_state_phase_async(
      wh::core::run_context &context, const graph_node_state_handlers *handlers,
      detail::state_runtime::state_phase phase, const std::string_view node_key,
      const graph_state_cause &cause, graph_process_state &process_state,
      graph_value payload, const node_path &runtime_path,
      detail::runtime_state::invoke_outputs &outputs,
      const wh::core::detail::any_resume_scheduler_t &graph_scheduler) const
      -> graph_sender;

  auto append_state_transition(detail::runtime_state::invoke_outputs &outputs,
                               const graph_call_scope &options,
                               const graph_state_transition_event &event,
                               const node_path &runtime_path) const
      -> void;

  auto append_state_transition(detail::runtime_state::invoke_outputs &outputs,
                               const graph_call_scope &options,
                               graph_state_transition_event &&event,
                               const node_path &runtime_path) const
      -> void;

  [[nodiscard]] auto evaluate_interrupt_hook(
      wh::core::run_context &context, const graph_interrupt_node_hook &hook,
      const std::string_view node_key, const graph_value &payload) const
      -> wh::core::result<std::optional<wh::core::interrupt_signal>>;

  [[nodiscard]] static auto
  make_missing_rerun_input_default(const node_contract contract)
      -> wh::core::result<graph_value>;

  auto resolve_missing_rerun_input(const node_contract input_contract) const
      -> wh::core::result<graph_value>;

  auto apply_runtime_resume_controls(
      wh::core::run_context &context,
      const detail::runtime_state::invoke_config &config) const
      -> wh::core::result<void>;

  auto maybe_restore_from_checkpoint(
      graph_value &input, wh::core::run_context &context,
      graph_state_table &state_table,
      detail::runtime_state::rerun_state &rerun_state,
      const detail::runtime_state::invoke_config &config,
      bool &skip_state_pre_handlers,
      detail::checkpoint_runtime::restore_scope scope,
      const node_path &runtime_path,
      detail::runtime_state::invoke_outputs &outputs,
      forwarded_checkpoint_map &forwarded_checkpoints) const
      -> wh::core::result<void>;

  auto maybe_persist_checkpoint(
      wh::core::run_context &context, const graph_state_table &state_table,
      detail::runtime_state::rerun_state &rerun_state,
      const detail::runtime_state::invoke_config &config,
      detail::runtime_state::invoke_outputs &outputs) const
      -> wh::core::result<void>;

  [[nodiscard]] auto resolve_edge_status_indexed(
      const indexed_edge &edge, const std::vector<node_state> &node_states,
      const std::vector<branch_state> &branch_states) const
      -> edge_status;

  [[nodiscard]] auto classify_node_readiness_indexed(
      const std::uint32_t node_id, const std::vector<node_state> &node_states,
      const std::vector<branch_state> &branch_states,
      const dynamic_bitset &output_valid,
      const scratch_buffer &scratch_buffer) const
      -> ready_state;

  [[nodiscard]] auto is_reader_eof_visible_for_fan_in_input(
      const std::uint32_t source_node_id,
      const scratch_buffer &scratch_buffer) const -> bool;

  [[nodiscard]] static auto collect_reader_value(
      graph_stream_reader reader, edge_limits limits,
      const wh::core::detail::any_resume_scheduler_t &graph_scheduler)
      -> graph_sender;

  [[nodiscard]] static constexpr auto
  needs_reader_lowering(const indexed_edge &edge) noexcept -> bool {
    return edge.source_output == node_contract::stream &&
           edge.target_input == node_contract::value;
  }

  [[nodiscard]] static auto make_reader_lowering(const indexed_edge &edge)
      -> wh::core::result<reader_lowering>;

  [[nodiscard]] static auto lower_reader(graph_stream_reader reader,
                                         reader_lowering lowering,
                                         wh::core::run_context &context,
                                         const wh::core::detail::any_resume_scheduler_t
                                             &graph_scheduler)
      -> graph_sender;

  [[nodiscard]] auto needs_reader_copy(const std::uint32_t node_id) const
      noexcept -> bool;

  [[nodiscard]] auto needs_reader_merge(
      const std::uint32_t node_id) const noexcept -> bool;

  [[nodiscard]] auto adapt_edge_output(const indexed_edge &edge,
                                       graph_value &source_output,
                                       wh::core::run_context &context) const
      -> wh::core::result<graph_value>;

  auto store_node_output(const std::uint32_t node_id,
                         scratch_buffer &scratch_buffer,
                         graph_value value) const -> wh::core::result<void>;

  [[nodiscard]] auto view_node_output(
      const std::uint32_t node_id,
      scratch_buffer &scratch_buffer) const
      -> wh::core::result<graph_value>;

  [[nodiscard]] auto cache_node_output(
      const std::uint32_t node_id,
      scratch_buffer &scratch_buffer) const
      -> wh::core::result<graph_value>;

  [[nodiscard]] auto take_node_output(
      const std::uint32_t node_id,
      scratch_buffer &scratch_buffer) const
      -> wh::core::result<graph_value>;

  auto prepare_reader_copies(
      const std::uint32_t source_node_id,
      scratch_buffer &scratch_buffer) const -> wh::core::result<void>;

  [[nodiscard]] auto merge_value_inputs(
      const std::vector<const graph_value *> &active_inputs,
      const detail::runtime_state::invoke_config &config,
      graph_value &scratch) const
      -> wh::core::result<bool>;

  [[nodiscard]] auto collect_input_lanes(
      const std::uint32_t node_id,
      const std::vector<node_state> &node_states,
      const std::vector<branch_state> &branch_states,
      const dynamic_bitset &output_valid) const -> std::vector<input_lane>;

  [[nodiscard]] auto build_missing_input(
      const compiled_node &node, const bool has_data_edge,
      scratch_buffer &scratch_buffer) const
      -> wh::core::result<resolved_input>;

  [[nodiscard]] static auto borrow_input(
      graph_value &value, const node_contract contract)
      -> wh::core::result<resolved_input>;

  [[nodiscard]] static auto own_input(
      graph_value value, const node_contract contract)
      -> wh::core::result<resolved_input>;

  [[nodiscard]] auto resolve_edge_value(
      const std::uint32_t edge_id, scratch_buffer &scratch_buffer,
      wh::core::run_context &context) const
      -> wh::core::result<graph_value *>;

  [[nodiscard]] auto resolve_edge_reader(
      const std::uint32_t edge_id, scratch_buffer &scratch_buffer,
      wh::core::run_context &context) const
      -> wh::core::result<graph_stream_reader *>;

  [[nodiscard]] auto take_edge_reader(
      const std::uint32_t edge_id, scratch_buffer &scratch_buffer,
      wh::core::run_context &context) const
      -> wh::core::result<graph_stream_reader>;

  [[nodiscard]] auto merged_reader(
      const std::uint32_t node_id, scratch_buffer &scratch_buffer) const
      -> wh::core::result<graph_stream_reader *>;

  auto update_merged_reader(
      const std::uint32_t node_id, scratch_buffer &scratch_buffer,
      const std::vector<input_lane> &lanes,
      wh::core::run_context &context) const -> wh::core::result<void>;

  auto refresh_merged_reader(
      const std::uint32_t node_id, scratch_buffer &scratch_buffer,
      const std::vector<node_state> &node_states,
      const std::vector<branch_state> &branch_states,
      wh::core::run_context &context) const -> wh::core::result<void>;

  [[nodiscard]] auto build_reader_input(
      const compiled_node &node, const std::uint32_t node_id,
      scratch_buffer &scratch_buffer, const std::vector<input_lane> &lanes,
      wh::core::run_context &context) const
      -> wh::core::result<resolved_input>;

  [[nodiscard]] auto build_value_input(
      const compiled_node &node, const bool has_data_edge,
      scratch_buffer &scratch_buffer, const std::vector<input_lane> &lanes,
      graph_value &scratch, wh::core::run_context &context,
      const detail::runtime_state::invoke_config &config) const
      -> wh::core::result<resolved_input>;

  [[nodiscard]] auto finish_value_input(
      const compiled_node &node, value_batch batch, graph_value &scratch,
      const detail::runtime_state::invoke_config &config) const
      -> wh::core::result<resolved_input>;

  auto refresh_source_readers(
      const std::uint32_t source_node_id, scratch_buffer &scratch_buffer,
      const std::vector<node_state> &node_states,
      const std::vector<branch_state> &branch_states,
      wh::core::run_context &context) const -> wh::core::result<void>;

  [[nodiscard]] auto build_node_input(
      const std::uint32_t node_id, scratch_buffer &scratch_buffer,
      const std::vector<node_state> &node_states,
      const std::vector<branch_state> &branch_states,
      graph_value &scratch, wh::core::run_context &context,
      const detail::runtime_state::invoke_config &config) const
      -> wh::core::result<resolved_input>;

  [[nodiscard]] auto build_node_input_sender(
      const std::uint32_t node_id, scratch_buffer &scratch_buffer,
      const std::vector<node_state> &node_states,
      const std::vector<branch_state> &branch_states,
      wh::core::run_context &context, node_frame *frame,
      const detail::runtime_state::invoke_config &config,
      const wh::core::detail::any_resume_scheduler_t &graph_scheduler) const
      -> graph_sender;

  [[nodiscard]] auto resolve_node_retry_budget(const std::uint32_t node_id) const
      -> std::size_t;

  [[nodiscard]] auto resolve_node_timeout_budget(const std::uint32_t node_id) const
      -> std::optional<std::chrono::milliseconds>;

  [[nodiscard]] auto resolve_node_parallel_gate(const std::uint32_t node_id) const
      -> std::size_t;

  [[nodiscard]] static auto resolve_branch_merge(
      const detail::runtime_state::invoke_config &config) noexcept
      -> graph_branch_merge;

  [[nodiscard]] static auto merge_branch_selected_nodes(
      const std::vector<std::uint32_t> &existing_sorted,
      std::vector<std::uint32_t> incoming_sorted,
      const graph_branch_merge strategy)
      -> wh::core::result<std::vector<std::uint32_t>>;

  auto commit_branch_selection(
      const std::uint32_t node_id,
      std::optional<std::vector<std::uint32_t>> selection,
      scratch_buffer &scratch,
      const detail::runtime_state::invoke_config &config) const
      -> wh::core::result<void>;

  [[nodiscard]] auto evaluate_branch_indexed(
      const std::uint32_t source_node_id, const graph_value &source_output,
      wh::core::run_context &context,
      const graph_call_scope &call_options) const
      -> wh::core::result<std::optional<std::vector<std::uint32_t>>>;

  [[nodiscard]] auto build_control_graph_index() const
      -> wh::core::result<control_graph_index>;

  [[nodiscard]] auto topo_sort(const control_graph_index &index) const
      -> wh::core::result<std::vector<std::string>>;

  [[nodiscard]] auto topo_sort_by_scc(const control_graph_index &index) const
      -> wh::core::result<std::vector<std::string>>;

  [[nodiscard]] auto is_reachable_start_to_end(const control_graph_index &index) const
      -> bool;

  [[nodiscard]] auto find_cycle_path(const control_graph_index &index) const
      -> std::optional<std::vector<std::string>>;

  [[nodiscard]] auto has_edge(const std::string &from, const std::string &to) const
      -> bool;

  auto init_reserved_nodes() -> void;

  auto release_cold_data_after_compile() -> void;

  auto ensure_writable() -> wh::core::result<void>;

  template <typename message_t>
    requires std::constructible_from<std::string, message_t &&>
  auto fail_fast(const wh::core::error_code code, message_t &&message)
      -> wh::core::result<void>;

  [[nodiscard]] constexpr auto is_cycle_allowed() const noexcept -> bool;

  [[nodiscard]] auto resolve_step_budget(
      const detail::runtime_state::invoke_config &config,
      const graph_call_scope &call_options) const
      -> wh::core::result<std::size_t>;

  /// Graph-level compile options.
  graph_compile_options options_{};
  /// Node registry keyed by stable node key.
  std::unordered_map<std::string, authored_node, detail::string_hash,
                     detail::string_equal>
      nodes_{};
  /// Runtime compiled nodes stored densely in stable compile order.
  std::vector<compiled_node> compiled_nodes_{};
  /// Stable node insertion order used by deterministic compile sort.
  std::vector<std::string> node_insertion_order_{};
  /// Stable node key -> insertion id index used by O(1) lookups.
  std::unordered_map<std::string, std::uint32_t, detail::string_hash,
                     detail::string_equal>
      node_id_index_{};
  /// Raw edge definitions.
  std::vector<graph_edge> edges_{};
  /// Branch source -> runtime selector/allowed target metadata.
  std::unordered_map<std::string, branch_definition, detail::string_hash,
                     detail::string_equal>
      branches_{};
  /// Diagnostics emitted by build/compile/runtime operations.
  std::vector<graph_diagnostic> diagnostics_{};
  /// Compile order produced by last successful compile.
  std::vector<std::string> compile_order_{};
  /// Precomputed runtime index used by invoke hot-path.
  compiled_execution_index compiled_execution_index_{};
  /// Lazy compile-stable graph snapshot used by diff/control-plane paths.
  mutable std::optional<graph_snapshot> snapshot_cache_{};
  /// One-time initializer guarding lazy snapshot materialization.
  mutable std::optional<std::once_flag> snapshot_once_{std::in_place};
  /// Compile-stable restore shape used by checkpoint validation/persist.
  graph_restore_shape restore_shape_{};
  /// Frozen flag set after successful compile.
  bool compiled_{false};
  /// First build/compile error preserved for fail-fast semantics.
  std::optional<wh::core::error_code> first_error_{};
};

} // namespace wh::compose
