// Defines the internal compose graph class declaration.
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
#include "wh/compose/graph/detail/graph_core.hpp"
#include "wh/compose/graph/detail/runtime/pending_inputs.hpp"
#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/compile_options.hpp"
#include "wh/compose/graph/detail/runtime/checkpoint/core.hpp"
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
class invoke_session;
class dag_runtime;
class pregel_runtime;
}

[[nodiscard]] auto start_bound_graph(
    const wh::compose::graph &graph, wh::core::run_context &context,
    graph_value &input, const graph_call_options *call_options,
    const node_path *path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const wh::core::detail::any_resume_scheduler_t &control_scheduler,
    const wh::core::detail::any_resume_scheduler_t &work_scheduler,
    const invoke_runtime::invoke_session *parent_state = nullptr,
    const graph_runtime_services *services = nullptr,
    graph_invoke_controls controls = {})
    -> graph_sender;

} // namespace detail

/// Mutable graph definition that compiles into a stable executable topology.
class graph {
  friend class detail::invoke_runtime::invoke_session;
  friend class detail::invoke_runtime::dag_runtime;
  friend class detail::invoke_runtime::pregel_runtime;

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

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
  [[nodiscard]] auto make_invoke_sender(request_t &&request,
                                        wh::core::run_context &context,
                                        graph_invoke_schedulers schedulers) const
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
  /// Returns compile-visible input gate exposed at the graph boundary.
  [[nodiscard]] auto boundary_input_gate() const noexcept -> input_gate;
  /// Returns compile-visible output gate exposed at the graph boundary.
  [[nodiscard]] auto boundary_output_gate() const noexcept -> output_gate;

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

  /// Registers one value-branch declaration by expanding to multiple edges.
  auto add_value_branch(const graph_value_branch &branch)
      -> wh::core::result<void>;

  /// Registers one value-branch declaration by expanding to multiple edges.
  auto add_value_branch(graph_value_branch &&branch) -> wh::core::result<void>;

  /// Registers one stream-branch declaration by expanding to multiple edges.
  auto add_stream_branch(const graph_stream_branch &branch)
      -> wh::core::result<void>;

  /// Registers one stream-branch declaration by expanding to multiple edges.
  auto add_stream_branch(graph_stream_branch &&branch)
      -> wh::core::result<void>;

  /// Runs compile validations and freezes graph structure.
  auto compile() -> wh::core::result<void>;

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
  /// Invokes graph with typed controls/services and returns one structured result.
  [[nodiscard]] auto invoke(wh::core::run_context &context,
                            request_t &&request) const -> auto;

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
  /// Invokes graph with explicit control/work schedulers.
  [[nodiscard]] auto invoke(wh::core::run_context &context, request_t &&request,
                            graph_invoke_schedulers schedulers) const -> auto;

private:
  [[nodiscard]] auto core() noexcept -> detail::graph_core & {
    return core_storage_;
  }

  [[nodiscard]] auto core() const noexcept -> const detail::graph_core & {
    return core_storage_;
  }

  detail::graph_core core_storage_{};

  friend auto detail::start_bound_graph(
      const graph &graph, wh::core::run_context &context,
      graph_value &input, const graph_call_options *call_options,
      const node_path *path_prefix, graph_process_state *parent_process_state,
      detail::runtime_state::invoke_outputs *nested_outputs,
      const wh::core::detail::any_resume_scheduler_t &control_scheduler,
      const wh::core::detail::any_resume_scheduler_t &work_scheduler,
      const detail::invoke_runtime::invoke_session *parent_state,
      const graph_runtime_services *services, graph_invoke_controls controls)
      -> graph_sender;
  friend auto detail::start_nested_graph(const graph &graph,
                                         wh::core::run_context &context,
                                         graph_value &input,
                                         const node_runtime &runtime)
      -> graph_sender;

  [[nodiscard]] static auto next_invoke_run_id() noexcept -> std::uint64_t;

  using dynamic_bitset = detail::graph_core::dynamic_bitset;
  using dag_node_phase = detail::graph_core::dag_node_phase;
  using dag_edge_status = detail::graph_core::dag_edge_status;
  using dag_ready_state = detail::graph_core::dag_ready_state;
  using pregel_ready_state = detail::graph_core::pregel_ready_state;
  using input_lane = detail::graph_core::input_lane;
  using input_lane_vector = detail::input_runtime::input_lane_vector;
  using input_lane_span = detail::input_runtime::input_lane_span;
  using dag_branch_state = detail::graph_core::dag_branch_state;
  using reader_lane_state = detail::graph_core::reader_lane_state;
  using reader_lowering = detail::graph_core::reader_lowering;
  using runtime_io_storage = detail::graph_core::runtime_io_storage;
  using dag_schedule_state = detail::graph_core::dag_schedule_state;
  using pregel_node_inputs = detail::graph_core::pregel_node_inputs;
  using pregel_delivery_store = detail::graph_core::pregel_delivery_store;
  using resolved_input = detail::graph_core::resolved_input;
  using value_input = detail::graph_core::value_input;
  using value_batch = detail::graph_core::value_batch;
  using edge_status = detail::graph_core::edge_status;
  using ready_state = detail::graph_core::ready_state;
  using branch_state = detail::graph_core::branch_state;
  using io_storage = detail::graph_core::io_storage;
  using dag_schedule = detail::graph_core::dag_schedule;
  using invoke_stage = detail::graph_core::invoke_stage;
  using attempt_id = detail::graph_core::attempt_id;
  using attempt_slot = detail::graph_core::attempt_slot;
  using state_step = detail::graph_core::state_step;
  using ready_action_kind = detail::graph_core::ready_action_kind;
  using ready_action = detail::graph_core::ready_action;
  using pregel_action = detail::graph_core::pregel_action;
  using indexed_edge = detail::graph_core::indexed_edge;
  using edge_flow = detail::graph_core::edge_flow;
  using indexed_value_branch_definition =
      detail::graph_core::indexed_value_branch_definition;
  using indexed_stream_branch_definition =
      detail::graph_core::indexed_stream_branch_definition;
  using csr_edge_index = detail::graph_core::csr_edge_index;
  using graph_index = detail::graph_core::graph_index;
  using output_plan = detail::graph_core::output_plan;
  using input_plan = detail::graph_core::input_plan;
  using graph_plan = detail::graph_core::graph_plan;
  using compiled_execution_index =
      detail::graph_core::compiled_execution_index;
  using control_graph_index = detail::graph_core::control_graph_index;
  using value_branch_definition =
      detail::graph_core::value_branch_definition;
  using stream_branch_definition =
      detail::graph_core::stream_branch_definition;

  struct value_output_consumers {
    std::vector<std::uint32_t> value_edges{};
    std::vector<std::uint32_t> stream_edges{};
    bool final_output{false};

    [[nodiscard]] auto owner_count() const noexcept -> std::size_t {
      return value_edges.size() + stream_edges.size() +
             static_cast<std::size_t>(final_output);
    }
  };

  template <typename node_t>
  auto add_node_impl(node_t &&node) -> wh::core::result<void>;

  [[nodiscard]] static constexpr auto
  default_edge_lowering_kind(const node_contract source_output,
                             const node_contract target_input) noexcept
      -> edge_lowering_kind;

  [[nodiscard]] auto resolve_edge_adapter(const graph_edge &edge) const
      -> wh::core::result<edge_adapter>;

  template <typename edge_t>
  auto add_edge_impl(edge_t &&edge) -> wh::core::result<void>;

  template <typename branch_t>
  auto add_value_branch_impl(branch_t &&branch) -> wh::core::result<void>;

  template <typename branch_t>
  auto add_stream_branch_impl(branch_t &&branch) -> wh::core::result<void>;

  auto validate_edges() -> wh::core::result<void>;

  auto validate_node_output_keys() -> wh::core::result<void>;

  auto validate_node_policy_overrides() -> wh::core::result<void>;

  auto validate_node_state_bindings() -> wh::core::result<void>;

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
      -> wh::core::result<compiled_node>;

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
  make_missing_pending_input_default(const node_contract contract)
      -> wh::core::result<graph_value>;

  auto resolve_missing_pending_input(const node_contract input_contract) const
      -> wh::core::result<graph_value>;

  auto apply_runtime_resume_controls(
      wh::core::run_context &context,
      const detail::runtime_state::invoke_config &config) const
      -> wh::core::result<void>;

  auto prepare_restore_checkpoint(
      wh::core::run_context &context,
      const detail::runtime_state::invoke_config &config,
      detail::checkpoint_runtime::restore_scope scope,
      const node_path &runtime_path,
      detail::runtime_state::invoke_outputs &outputs,
      forwarded_checkpoint_map &forwarded_checkpoints) const
      -> wh::core::result<
          std::optional<detail::checkpoint_runtime::prepared_restore>>;

  auto maybe_persist_checkpoint(
      wh::core::run_context &context, checkpoint_state checkpoint,
      const detail::runtime_state::invoke_config &config,
      detail::runtime_state::invoke_outputs &outputs) const
      -> wh::core::result<void>;

  [[nodiscard]] auto resolve_edge_status_indexed(
      const indexed_edge &edge, const std::vector<dag_node_phase> &dag_node_phases,
      const std::vector<branch_state> &branch_states) const
      -> edge_status;

  [[nodiscard]] auto classify_node_readiness_indexed(
      const std::uint32_t node_id, const std::vector<dag_node_phase> &dag_node_phases,
      const std::vector<branch_state> &branch_states,
      const dynamic_bitset &output_valid) const
      -> ready_state;

  [[nodiscard]] auto classify_pregel_node_readiness(
      const std::uint32_t node_id,
      const pregel_node_inputs &inputs) const -> pregel_ready_state;

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

  [[nodiscard]] auto needs_reader_merge(
      const std::uint32_t node_id) const noexcept -> bool;

  [[nodiscard]] auto adapt_edge_output(const indexed_edge &edge,
                                       graph_value &source_output,
                                       wh::core::run_context &context) const
      -> wh::core::result<graph_value>;

  [[nodiscard]] auto plan_value_output_consumers(
      const std::uint32_t source_node_id,
      const std::optional<std::vector<std::uint32_t>> &selection) const
      -> value_output_consumers;

  [[nodiscard]] auto lower_value_output_reader(
      const indexed_edge &edge, graph_value value,
      wh::core::run_context &context) const
      -> wh::core::result<graph_stream_reader>;

  auto store_node_output(const std::uint32_t node_id,
                         io_storage &io_storage,
                         graph_value value) const -> wh::core::result<void>;

  auto commit_value_output(
      const std::uint32_t source_node_id, io_storage &io_storage,
      graph_value value,
      const std::optional<std::vector<std::uint32_t>> &selection,
      wh::core::run_context &context) const -> wh::core::result<void>;

  [[nodiscard]] auto view_node_output(
      const std::uint32_t node_id,
      io_storage &io_storage) const
      -> wh::core::result<graph_value>;

  [[nodiscard]] auto take_node_output(
      const std::uint32_t node_id,
      io_storage &io_storage) const
      -> wh::core::result<graph_value>;

  auto commit_stream_output(
      const std::uint32_t source_node_id,
      io_storage &io_storage, graph_stream_reader reader,
      const std::optional<std::vector<std::uint32_t>> &selection) const
      -> wh::core::result<void>;

  [[nodiscard]] auto collect_input_lanes(
      const std::uint32_t node_id,
      const std::vector<dag_node_phase> &dag_node_phases,
      const std::vector<branch_state> &branch_states,
      const dynamic_bitset &output_valid) const -> input_lane_vector;

  [[nodiscard]] auto build_missing_input(const compiled_node &node) const
      -> wh::core::result<resolved_input>;

  [[nodiscard]] static auto borrow_input(
      graph_value &value, const node_contract contract)
      -> wh::core::result<resolved_input>;

  [[nodiscard]] static auto own_input(
      graph_value value, const node_contract contract)
      -> wh::core::result<resolved_input>;

  [[nodiscard]] auto resolve_edge_value(
      const std::uint32_t edge_id, io_storage &io_storage,
      wh::core::run_context &context) const
      -> wh::core::result<graph_value *>;

  [[nodiscard]] auto resolve_edge_reader(
      const std::uint32_t edge_id, io_storage &io_storage) const
      -> wh::core::result<graph_stream_reader *>;

  [[nodiscard]] auto take_edge_reader(
      const std::uint32_t edge_id, io_storage &io_storage) const
      -> wh::core::result<graph_stream_reader>;

  [[nodiscard]] auto merged_reader(
      const std::uint32_t node_id, io_storage &io_storage) const
      -> wh::core::result<graph_stream_reader *>;

  auto update_merged_reader(
      const std::uint32_t node_id, io_storage &io_storage,
      input_lane_span lanes) const -> wh::core::result<void>;

  auto refresh_merged_reader(
      const std::uint32_t node_id, io_storage &io_storage,
      const std::vector<dag_node_phase> &dag_node_phases,
      const std::vector<branch_state> &branch_states) const
      -> wh::core::result<void>;

  [[nodiscard]] auto build_reader_input(
      const compiled_node &node, const std::uint32_t node_id,
      io_storage &io_storage, input_lane_span lanes) const
      -> wh::core::result<resolved_input>;

  [[nodiscard]] auto build_value_input(
      const compiled_node &node, io_storage &io_storage,
      input_lane_span lanes,
      wh::core::run_context &context) const
      -> wh::core::result<resolved_input>;

  [[nodiscard]] auto finish_value_input(
      const compiled_node &node, value_batch batch) const
      -> wh::core::result<resolved_input>;

  auto refresh_source_readers(
      const std::uint32_t source_node_id, io_storage &io_storage,
      const std::vector<dag_node_phase> &dag_node_phases,
      const std::vector<branch_state> &branch_states) const
      -> wh::core::result<void>;

  auto reset_pregel_source_caches(
      const std::uint32_t source_node_id,
      io_storage &io_storage) const -> void;

  auto seed_pregel_successors(
      const std::uint32_t source_node_id,
      const std::optional<std::vector<std::uint32_t>> &selection,
      pregel_delivery_store &deliveries) const -> void;

  auto stage_pregel_successors(
      const std::uint32_t source_node_id,
      const std::optional<std::vector<std::uint32_t>> &selection,
      pregel_delivery_store &deliveries) const -> void;

  [[nodiscard]] auto build_node_input(
      const std::uint32_t node_id, io_storage &io_storage,
      const std::vector<dag_node_phase> &dag_node_phases,
      const std::vector<branch_state> &branch_states,
      graph_value &scratch, wh::core::run_context &context,
      const detail::runtime_state::invoke_config &config) const
      -> wh::core::result<resolved_input>;

  [[nodiscard]] auto build_node_input_sender(
      const std::uint32_t node_id, io_storage &io_storage,
      const std::vector<dag_node_phase> &dag_node_phases,
      const std::vector<branch_state> &branch_states,
      wh::core::run_context &context, attempt_slot *slot,
      const detail::runtime_state::invoke_config &config,
      const wh::core::detail::any_resume_scheduler_t &graph_scheduler) const
      -> graph_sender;

  [[nodiscard]] auto build_pregel_node_input_sender(
      const std::uint32_t node_id, const pregel_node_inputs &inputs,
      io_storage &io_storage, wh::core::run_context &context,
      attempt_slot *slot, const detail::runtime_state::invoke_config &config,
      const wh::core::detail::any_resume_scheduler_t &graph_scheduler) const
      -> graph_sender;

  [[nodiscard]] auto resolve_node_retry_budget(const std::uint32_t node_id) const
      -> std::size_t;

  [[nodiscard]] auto resolve_node_timeout_budget(const std::uint32_t node_id) const
      -> std::optional<std::chrono::milliseconds>;

  [[nodiscard]] auto resolve_node_parallel_gate(const std::uint32_t node_id) const
      -> std::size_t;

  [[nodiscard]] auto resolve_node_sync_dispatch(const std::uint32_t node_id) const
      -> sync_dispatch;

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
      dag_schedule &dag_schedule,
      const detail::runtime_state::invoke_config &config) const
      -> wh::core::result<void>;

  [[nodiscard]] auto evaluate_value_branch_indexed(
      const std::uint32_t source_node_id, const graph_value &source_output,
      wh::core::run_context &context,
      const graph_call_scope &call_options) const
      -> wh::core::result<std::optional<std::vector<std::uint32_t>>>;

  [[nodiscard]] auto evaluate_stream_branch_indexed(
      const std::uint32_t source_node_id, graph_stream_reader &source_output,
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

};

} // namespace wh::compose
