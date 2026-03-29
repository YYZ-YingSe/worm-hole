// Defines out-of-line graph compile/index helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/contract_check.hpp"

namespace wh::compose {

namespace detail {

[[nodiscard]] inline auto to_snapshot_compile_options(
    const graph_compile_options &options) -> graph_snapshot_compile_options {
  return graph_snapshot_compile_options{
      .name = options.name,
      .boundary = options.boundary,
      .mode = options.mode,
      .eager = options.eager,
      .max_steps = options.max_steps,
      .retain_cold_data = options.retain_cold_data,
      .trigger_mode = options.trigger_mode,
      .fan_in_policy = options.fan_in_policy,
      .retry_budget = options.retry_budget,
      .node_timeout = options.node_timeout,
      .max_parallel_nodes = options.max_parallel_nodes,
      .max_parallel_per_node = options.max_parallel_per_node,
      .enable_local_state_generation = options.enable_local_state_generation,
      .has_compile_callback = static_cast<bool>(options.compile_callback),
  };
}

[[nodiscard]] inline auto edge_snapshot_less(const graph_snapshot_edge &lhs,
                                             const graph_snapshot_edge &rhs)
    noexcept -> bool {
  return std::tie(lhs.from, lhs.to) < std::tie(rhs.from, rhs.to);
}

[[nodiscard]] inline auto branch_snapshot_less(
    const graph_snapshot_branch &lhs,
    const graph_snapshot_branch &rhs) noexcept -> bool {
  return lhs.from < rhs.from;
}

[[nodiscard]] inline auto node_snapshot_less(const graph_snapshot_node &lhs,
                                             const graph_snapshot_node &rhs)
    noexcept -> bool {
  return lhs.key < rhs.key;
}

[[nodiscard]] inline auto to_snapshot_edge(const graph_edge &edge)
    -> graph_snapshot_edge {
  return graph_snapshot_edge{
      .from = edge.from,
      .to = edge.to,
      .no_control = edge.options.no_control,
      .no_data = edge.options.no_data,
      .adapter_kind = edge.options.adapter.kind,
      .has_custom_value_to_stream =
          edge.options.adapter.custom.value_to_stream.has_value(),
      .has_custom_stream_to_value =
          edge.options.adapter.custom.stream_to_value.has_value(),
      .limits = edge.options.limits,
  };
}

} // namespace detail

inline graph::graph() {
  init_reserved_nodes();
}

inline graph::graph(const graph_boundary boundary) {
  options_.boundary = boundary;
  init_reserved_nodes();
}

inline graph::graph(const graph_compile_options &options)
    : options_(options) {
  init_reserved_nodes();
}

inline graph::graph(graph_compile_options &&options)
    : options_(std::move(options)) {
  init_reserved_nodes();
}

inline graph::graph(const graph_boundary boundary,
                    const graph_compile_options &options)
    : options_(options) {
  options_.boundary = boundary;
  init_reserved_nodes();
}

inline graph::graph(const graph_boundary boundary,
                    graph_compile_options &&options)
    : options_(std::move(options)) {
  options_.boundary = boundary;
  init_reserved_nodes();
}

inline graph::graph(const graph &other)
    : options_(other.options_),
      nodes_(other.nodes_),
      compiled_nodes_(other.compiled_nodes_),
      node_insertion_order_(other.node_insertion_order_),
      node_id_index_(other.node_id_index_),
      edges_(other.edges_),
      branches_(other.branches_),
      diagnostics_(other.diagnostics_),
      compile_order_(other.compile_order_),
      compiled_execution_index_(other.compiled_execution_index_),
      snapshot_cache_(other.snapshot_cache_),
      snapshot_once_(std::in_place),
      restore_shape_(other.restore_shape_),
      compiled_(other.compiled_),
      first_error_(other.first_error_) {
  rebind_compiled_execution_index_nodes();
}

inline graph::graph(graph &&other) noexcept
    : options_(std::move(other.options_)),
      nodes_(std::move(other.nodes_)),
      compiled_nodes_(std::move(other.compiled_nodes_)),
      node_insertion_order_(std::move(other.node_insertion_order_)),
      node_id_index_(std::move(other.node_id_index_)),
      edges_(std::move(other.edges_)),
      branches_(std::move(other.branches_)),
      diagnostics_(std::move(other.diagnostics_)),
      compile_order_(std::move(other.compile_order_)),
      compiled_execution_index_(std::move(other.compiled_execution_index_)),
      snapshot_cache_(std::move(other.snapshot_cache_)),
      snapshot_once_(std::in_place),
      restore_shape_(std::move(other.restore_shape_)),
      compiled_(other.compiled_),
      first_error_(std::move(other.first_error_)) {
  rebind_compiled_execution_index_nodes();
  other.snapshot_cache_.reset();
  other.snapshot_once_.emplace();
}

inline auto graph::operator=(const graph &other) -> graph & {
  if (this == &other) {
    return *this;
  }
  options_ = other.options_;
  nodes_ = other.nodes_;
  compiled_nodes_ = other.compiled_nodes_;
  node_insertion_order_ = other.node_insertion_order_;
  node_id_index_ = other.node_id_index_;
  edges_ = other.edges_;
  branches_ = other.branches_;
  diagnostics_ = other.diagnostics_;
  compile_order_ = other.compile_order_;
  compiled_execution_index_ = other.compiled_execution_index_;
  snapshot_cache_ = other.snapshot_cache_;
  snapshot_once_.emplace();
  restore_shape_ = other.restore_shape_;
  compiled_ = other.compiled_;
  first_error_ = other.first_error_;
  rebind_compiled_execution_index_nodes();
  return *this;
}

inline auto graph::operator=(graph &&other) noexcept -> graph & {
  if (this == &other) {
    return *this;
  }
  options_ = std::move(other.options_);
  nodes_ = std::move(other.nodes_);
  compiled_nodes_ = std::move(other.compiled_nodes_);
  node_insertion_order_ = std::move(other.node_insertion_order_);
  node_id_index_ = std::move(other.node_id_index_);
  edges_ = std::move(other.edges_);
  branches_ = std::move(other.branches_);
  diagnostics_ = std::move(other.diagnostics_);
  compile_order_ = std::move(other.compile_order_);
  compiled_execution_index_ = std::move(other.compiled_execution_index_);
  snapshot_cache_ = std::move(other.snapshot_cache_);
  snapshot_once_.emplace();
  restore_shape_ = std::move(other.restore_shape_);
  compiled_ = other.compiled_;
  first_error_ = std::move(other.first_error_);
  rebind_compiled_execution_index_nodes();
  other.snapshot_cache_.reset();
  other.snapshot_once_.emplace();
  return *this;
}

inline auto graph::options() const noexcept -> const graph_compile_options & {
  return options_;
}

inline auto graph::boundary() const noexcept -> const graph_boundary & {
  return options_.boundary;
}

inline auto graph::compile_options_snapshot() const -> graph_compile_options {
  return options_;
}

inline auto graph::compiled() const noexcept -> bool { return compiled_; }

inline auto graph::snapshot() const -> graph_snapshot {
  if (!compiled_) {
    return build_snapshot();
  }
  return snapshot_view();
}

inline auto graph::snapshot_view() const -> const graph_snapshot & {
  std::call_once(*snapshot_once_, [this]() {
    if (!snapshot_cache_.has_value()) {
      snapshot_cache_.emplace(build_snapshot());
    }
  });
  return *snapshot_cache_;
}

inline auto graph::restore_shape() const noexcept
    -> const graph_restore_shape & {
  return restore_shape_;
}

inline auto graph::diagnostics() const noexcept
    -> const std::vector<graph_diagnostic> & {
  return diagnostics_;
}

inline auto graph::compile_order() const noexcept
    -> const std::vector<std::string> & {
  return compile_order_;
}

inline auto graph::node_id(const std::string_view key) const
    -> wh::core::result<std::uint32_t> {
  if (!compiled_execution_index_.index.key_to_id.empty()) {
    const auto runtime_iter =
        compiled_execution_index_.index.key_to_id.find(key);
    if (runtime_iter != compiled_execution_index_.index.key_to_id.end()) {
      return runtime_iter->second;
    }
  }
  const auto iter = node_id_index_.find(key);
  if (iter != node_id_index_.end()) {
    return iter->second;
  }
  return wh::core::result<std::uint32_t>::failure(wh::core::errc::not_found);
}

inline auto graph::compiled_node_by_key(const std::string_view key) const
    -> wh::core::result<std::reference_wrapper<const compiled_node>> {
  if (!compiled_) {
    return wh::core::result<std::reference_wrapper<const compiled_node>>::failure(
        wh::core::errc::contract_violation);
  }
  const auto iter = compiled_execution_index_.index.key_to_id.find(key);
  if (iter == compiled_execution_index_.index.key_to_id.end() ||
      iter->second >= compiled_nodes_.size()) {
    return wh::core::result<std::reference_wrapper<const compiled_node>>::failure(
        wh::core::errc::not_found);
  }
  return std::cref(compiled_nodes_[iter->second]);
}

inline auto graph::compile() -> wh::core::result<void> {
  if (compiled_) {
    return fail_fast(wh::core::errc::contract_violation,
                     "graph already compiled");
  }
  if (first_error_.has_value()) {
    return wh::core::result<void>::failure(*first_error_);
  }

  diagnostics_.push_back(graph_diagnostic{
      .code = wh::core::errc::ok,
      .message = "compile_options:" +
                 serialize_graph_compile_options(compile_options_snapshot()),
  });

  if (options_.max_steps == 0U) {
    return fail_fast(wh::core::errc::invalid_argument,
                     "max_steps must be greater than zero");
  }
  if (options_.max_parallel_nodes == 0U) {
    return fail_fast(wh::core::errc::invalid_argument,
                     "max_parallel_nodes must be greater than zero");
  }
  if (options_.max_parallel_per_node == 0U) {
    return fail_fast(wh::core::errc::invalid_argument,
                     "max_parallel_per_node must be greater than zero");
  }
  if (options_.node_timeout.has_value() &&
      options_.node_timeout.value() <= std::chrono::milliseconds{0}) {
    return fail_fast(wh::core::errc::invalid_argument,
                     "node_timeout must be greater than zero");
  }

  auto validated = validate_edges();
  if (validated.has_error()) {
    return validated;
  }
  auto output_keys = validate_node_output_keys();
  if (output_keys.has_error()) {
    return output_keys;
  }
  auto node_policy = validate_node_policy_overrides();
  if (node_policy.has_error()) {
    return node_policy;
  }
  auto control_index = build_control_graph_index();
  if (control_index.has_error()) {
    return wh::core::result<void>::failure(control_index.error());
  }
  if (!is_reachable_start_to_end(control_index.value())) {
    return fail_fast(wh::core::errc::not_found,
                     "no reachable path from START to END");
  }
  if (!is_cycle_allowed()) {
    auto cycle = find_cycle_path(control_index.value());
    if (cycle.has_value()) {
      std::string cycle_text{};
      for (std::size_t index = 0U; index < cycle->size(); ++index) {
        if (index > 0U) {
          cycle_text += "->";
        }
        cycle_text += (*cycle)[index];
      }
      return fail_fast(wh::core::errc::contract_violation,
                       "cycle detected: " + cycle_text);
    }
  }

  auto sorted = topo_sort(control_index.value());
  if (sorted.has_error()) {
    return wh::core::result<void>::failure(sorted.error());
  }
  compile_order_ = std::move(sorted).value();
  auto prepared = build_compiled_execution_index();
  if (prepared.has_error()) {
    return prepared;
  }
  auto contracts = validate_contracts();
  if (contracts.has_error()) {
    return contracts;
  }
  if (static_cast<bool>(options_.compile_callback)) {
    auto compile_callback_status =
        options_.compile_callback(build_compile_info_snapshot());
    if (compile_callback_status.has_error()) {
      return fail_fast(compile_callback_status.error(),
                       "compile callback failed");
    }
  }
  node_id_index_.clear();
  node_id_index_.rehash(0U);
  if (!options_.retain_cold_data) {
    release_cold_data_after_compile();
  }
  snapshot_cache_.reset();
  snapshot_once_.emplace();
  restore_shape_ = build_restore_shape();
  compiled_ = true;
  return {};
}

inline auto graph::validate_edges() -> wh::core::result<void> {
  for (const auto &edge : edges_) {
    if (!nodes_.contains(edge.from) || !nodes_.contains(edge.to)) {
      return fail_fast(wh::core::errc::not_found,
                       "edge endpoint not found: " + edge.from + "->" + edge.to);
    }
    if (edge.options.no_control && edge.options.no_data) {
      return fail_fast(wh::core::errc::invalid_argument,
                       "edge noControl and noData cannot both be true: " +
                           edge.from + "->" + edge.to);
    }
    auto adapter = resolve_edge_adapter(edge);
    if (adapter.has_error()) {
      return fail_fast(wh::core::errc::contract_violation,
                       "edge contract mismatch: " + edge.from + "->" + edge.to +
                           " (" +
                           std::string{to_string(authored_output_contract(
                               nodes_.at(edge.from)))} +
                           " -> " +
                           std::string{to_string(authored_input_contract(
                               nodes_.at(edge.to)))} +
                           ")");
    }
  }
  return {};
}

inline auto graph::validate_node_output_keys() -> wh::core::result<void> {
  std::unordered_map<std::string, std::string, detail::string_hash,
                     detail::string_equal>
      output_owner{};
  output_owner.reserve(nodes_.size());
  for (const auto &[key, node] : nodes_) {
    if (key == graph_start_node_key || key == graph_end_node_key) {
      continue;
    }
    const auto &options = authored_options(node);
    if (options.output_key.empty()) {
      continue;
    }
    const auto inserted = output_owner.emplace(options.output_key, key);
    if (!inserted.second) {
      return fail_fast(wh::core::errc::already_exists,
                       "duplicate output key: " + options.output_key);
    }
  }
  return {};
}

inline auto graph::validate_node_policy_overrides() -> wh::core::result<void> {
  bool requires_state_generation = false;
  for (const auto &[key, node] : nodes_) {
    if (key == graph_start_node_key || key == graph_end_node_key) {
      continue;
    }
    const auto &options = authored_options(node);
    requires_state_generation =
        requires_state_generation || options.state_handlers.any();
    if (options.max_parallel_override.has_value() &&
        *options.max_parallel_override == 0U) {
      return fail_fast(wh::core::errc::invalid_argument,
                       "node max_parallel_override must be greater than zero: " +
                           key);
    }
    if (options.timeout_override.has_value() &&
        options.timeout_override.value() <= std::chrono::milliseconds{0}) {
      return fail_fast(wh::core::errc::invalid_argument,
                       "node timeout_override must be greater than zero: " + key);
    }
    if (options.retry_window_override.has_value() &&
        options.retry_window_override.value() <= std::chrono::milliseconds{0}) {
      return fail_fast(
          wh::core::errc::invalid_argument,
          "node retry_window_override must be greater than zero: " + key);
    }

    if (options.retry_window_override.has_value()) {
      const auto effective_timeout =
          options.timeout_override.has_value() ? options.timeout_override
                                               : options_.node_timeout;
      if (!effective_timeout.has_value()) {
        return fail_fast(
            wh::core::errc::invalid_argument,
            "node retry_window_override requires timeout budget: " + key);
      }
      if (*effective_timeout >= *options.retry_window_override) {
        return fail_fast(
            wh::core::errc::invalid_argument,
            "node timeout must be smaller than retry_window_override: " + key);
      }
    }
  }
  if (requires_state_generation && !options_.enable_local_state_generation) {
    return fail_fast(
        wh::core::errc::contract_violation,
        "state handlers require enable_local_state_generation=true");
  }
  return {};
}

inline auto graph::to_compile_node_options_info(
    const graph_add_node_options &options, const node_contract input_contract)
    -> graph_compile_node_options_info {
  return graph_compile_node_options_info{
      .name = options.name,
      .type = options.type,
      .input_key = options.input_key,
      .output_key = options.output_key,
      .observation =
          graph_compile_node_observation_info{
              .callbacks_enabled = options.observation.callbacks_enabled,
              .allow_invoke_override =
                  options.observation.allow_invoke_override,
              .local_callback_count =
                  options.observation.local_callbacks.size(),
          },
      .label = options.label,
      .input_contract = static_cast<std::uint8_t>(input_contract),
      .allow_no_control = options.allow_no_control,
      .allow_no_data = options.allow_no_data,
      .retry_budget_override = options.retry_budget_override,
      .timeout_override = options.timeout_override,
      .retry_window_override = options.retry_window_override,
      .max_parallel_override = options.max_parallel_override,
      .state_handlers = options.state_handlers,
  };
}

inline auto graph::build_compile_info_snapshot() const -> graph_compile_info {
  graph_compile_info info{};
  info.name = options_.name;
  info.mode = options_.mode;
  info.eager = options_.eager;
  info.max_steps = options_.max_steps;
  info.trigger_mode = options_.trigger_mode;
  info.fan_in_policy = options_.fan_in_policy;
  info.retry_budget = options_.retry_budget;
  info.node_timeout = options_.node_timeout;
  info.max_parallel_nodes = options_.max_parallel_nodes;
  info.max_parallel_per_node = options_.max_parallel_per_node;
  info.state_generator_enabled = options_.enable_local_state_generation;
  info.compile_order = compile_order_;
  info.node_key_to_id.reserve(node_id_index_.size());
  for (const auto &[key, node_id] : node_id_index_) {
    info.node_key_to_id.emplace(key, node_id);
  }

  info.nodes.reserve(node_insertion_order_.size());
  for (std::size_t node_index = 0U; node_index < node_insertion_order_.size();
       ++node_index) {
    const auto &node_key = node_insertion_order_[node_index];
    const auto node_iter = nodes_.find(node_key);
    if (node_iter == nodes_.end()) {
      continue;
    }
    info.nodes.push_back(graph_compile_node_info{
        .key = node_key,
        .node_id = static_cast<std::uint32_t>(node_index),
        .has_sender = true,
        .has_subgraph =
            authored_options(node_iter->second).subgraph_compile_info.has_value() ||
            std::holds_alternative<subgraph_node>(node_iter->second),
        .field_mapping =
            graph_compile_field_mapping_info{
                .input_key = authored_options(node_iter->second).input_key,
                .output_key = authored_options(node_iter->second).output_key,
            },
        .options = to_compile_node_options_info(
            authored_options(node_iter->second),
            authored_input_contract(node_iter->second)),
    });
    if (authored_options(node_iter->second).subgraph_compile_info.has_value()) {
      info.subgraphs.insert_or_assign(
          node_key, *authored_options(node_iter->second).subgraph_compile_info);
    }
  }

  info.edges = edges_;
  info.control_edges.reserve(edges_.size());
  info.data_edges.reserve(edges_.size());
  for (const auto &edge : edges_) {
    if (!edge.options.no_control) {
      info.control_edges.push_back(edge);
    }
    if (!edge.options.no_data) {
      info.data_edges.push_back(edge);
    }
  }

  std::vector<std::string> branch_keys{};
  branch_keys.reserve(branches_.size());
  for (const auto &[source, _] : branches_) {
    branch_keys.push_back(source);
  }
  std::sort(branch_keys.begin(), branch_keys.end());
  info.branches.reserve(branch_keys.size());
  for (const auto &source : branch_keys) {
    const auto branch_iter = branches_.find(source);
    if (branch_iter == branches_.end()) {
      continue;
    }
    info.branches.push_back(graph_compile_branch_info{
        .from = source,
        .end_nodes = branch_iter->second.end_nodes,
    });
  }
  return info;
}

inline auto graph::build_snapshot() const -> graph_snapshot {
  graph_snapshot snapshot{};
  snapshot.compile_options = detail::to_snapshot_compile_options(options_);

  if (!compiled_execution_index_.index.id_to_key.empty() &&
      !compiled_nodes_.empty()) {
    snapshot.node_id_to_key = compiled_execution_index_.index.id_to_key;
    snapshot.node_key_to_id.reserve(
        compiled_execution_index_.index.key_to_id.size());
    for (const auto &[key, node_id] :
         compiled_execution_index_.index.key_to_id) {
      snapshot.node_key_to_id.emplace(key, node_id);
    }

    snapshot.nodes.reserve(compiled_nodes_.size());
    for (std::size_t node_id = 0U; node_id < compiled_nodes_.size(); ++node_id) {
      const auto &node = compiled_nodes_[node_id];
      if (node.meta.key == graph_start_node_key ||
          node.meta.key == graph_end_node_key) {
        continue;
      }
      snapshot.nodes.push_back(graph_snapshot_node{
          .key = node.meta.key,
          .node_id = static_cast<std::uint32_t>(node_id),
          .kind = node.meta.kind,
          .exec_mode = node.meta.exec_mode,
          .exec_origin = node.meta.exec_origin,
          .input_contract = node.meta.input_contract,
          .output_contract = node.meta.output_contract,
          .options = to_compile_node_options_info(node.meta.options,
                                                  node.meta.input_contract),
      });
      if (node.meta.subgraph_snapshot.has_value()) {
        snapshot.subgraphs.emplace(node.meta.key, *node.meta.subgraph_snapshot);
      }
    }
    std::sort(snapshot.nodes.begin(), snapshot.nodes.end(),
              detail::node_snapshot_less);

    snapshot.edges.reserve(compiled_execution_index_.index.indexed_edges.size());
    for (const auto &edge : compiled_execution_index_.index.indexed_edges) {
      snapshot.edges.push_back(graph_snapshot_edge{
          .from = compiled_execution_index_.index.id_to_key[edge.from],
          .to = compiled_execution_index_.index.id_to_key[edge.to],
          .no_control = edge.no_control,
          .no_data = edge.no_data,
          .adapter_kind = edge.adapter.kind,
          .has_custom_value_to_stream =
              edge.adapter.custom.value_to_stream.has_value(),
          .has_custom_stream_to_value =
              edge.adapter.custom.stream_to_value.has_value(),
          .limits = edge.limits,
      });
    }
    std::sort(snapshot.edges.begin(), snapshot.edges.end(),
              detail::edge_snapshot_less);

    snapshot.branches.reserve(
        compiled_execution_index_.index.has_branch_by_source.size());
    for (std::uint32_t source_id = 0U;
         source_id < static_cast<std::uint32_t>(
                         compiled_execution_index_.index
                             .has_branch_by_source.size());
         ++source_id) {
      const auto *branch =
          compiled_execution_index_.index.branch_for_source(source_id);
      if (branch == nullptr) {
        continue;
      }
      auto branch_snapshot = graph_snapshot_branch{
          .from = compiled_execution_index_.index.id_to_key[source_id],
      };
      branch_snapshot.end_nodes.reserve(branch->end_nodes_sorted.size());
      for (const auto node_id : branch->end_nodes_sorted) {
        branch_snapshot.end_nodes.push_back(
            compiled_execution_index_.index.id_to_key[node_id]);
      }
      std::sort(branch_snapshot.end_nodes.begin(), branch_snapshot.end_nodes.end());
      snapshot.branches.push_back(std::move(branch_snapshot));
    }
    std::sort(snapshot.branches.begin(), snapshot.branches.end(),
              detail::branch_snapshot_less);
    return snapshot;
  }

  snapshot.node_id_to_key = node_insertion_order_;
  snapshot.node_key_to_id.reserve(node_id_index_.size());
  for (const auto &[key, node_id] : node_id_index_) {
    snapshot.node_key_to_id.emplace(key, node_id);
  }

  snapshot.nodes.reserve(nodes_.size());
  for (std::size_t node_index = 0U; node_index < node_insertion_order_.size();
       ++node_index) {
    const auto &node_key = node_insertion_order_[node_index];
    const auto node_iter = nodes_.find(node_key);
    if (node_iter == nodes_.end() || node_key == graph_start_node_key ||
        node_key == graph_end_node_key) {
      continue;
    }
    snapshot.nodes.push_back(graph_snapshot_node{
        .key = node_key,
        .node_id = static_cast<std::uint32_t>(node_index),
        .kind = authored_kind(node_iter->second),
        .exec_mode = std::visit(
            [](const auto &value) -> node_exec_mode { return value.exec_mode(); },
            node_iter->second),
        .exec_origin = std::visit(
            [](const auto &value) -> node_exec_origin {
              return value.exec_origin();
            },
            node_iter->second),
        .input_contract = authored_input_contract(node_iter->second),
        .output_contract = authored_output_contract(node_iter->second),
        .options = to_compile_node_options_info(
            authored_options(node_iter->second),
            authored_input_contract(node_iter->second)),
    });
  }
  std::sort(snapshot.nodes.begin(), snapshot.nodes.end(),
            detail::node_snapshot_less);

  snapshot.edges.reserve(edges_.size());
  for (const auto &edge : edges_) {
    snapshot.edges.push_back(detail::to_snapshot_edge(edge));
  }
  std::sort(snapshot.edges.begin(), snapshot.edges.end(),
            detail::edge_snapshot_less);

  snapshot.branches.reserve(branches_.size());
  for (const auto &[source, branch] : branches_) {
    auto branch_snapshot = graph_snapshot_branch{
        .from = source,
        .end_nodes = branch.end_nodes,
    };
    std::sort(branch_snapshot.end_nodes.begin(), branch_snapshot.end_nodes.end());
    snapshot.branches.push_back(std::move(branch_snapshot));
  }
  std::sort(snapshot.branches.begin(), snapshot.branches.end(),
            detail::branch_snapshot_less);
  return snapshot;
}

inline auto graph::build_restore_shape() const -> graph_restore_shape {
  graph_restore_shape shape{};
  shape.options = graph_restore_options{
      .boundary = options_.boundary,
      .mode = options_.mode,
      .trigger_mode = options_.trigger_mode,
      .fan_in_policy = options_.fan_in_policy,
  };

  if (!compiled_execution_index_.index.id_to_key.empty() &&
      !compiled_nodes_.empty()) {
    shape.nodes.reserve(compiled_nodes_.size());
    for (const auto &node : compiled_nodes_) {
      if (node.meta.key == graph_start_node_key ||
          node.meta.key == graph_end_node_key) {
        continue;
      }
      shape.nodes.push_back(graph_restore_node{
          .key = node.meta.key,
          .kind = node.meta.kind,
          .input_contract = node.meta.input_contract,
          .allow_no_control = node.meta.options.allow_no_control,
          .allow_no_data = node.meta.options.allow_no_data,
      });
      if (node.meta.subgraph_restore_shape.has_value()) {
        shape.subgraphs.emplace(node.meta.key, *node.meta.subgraph_restore_shape);
      } else if (node.meta.subgraph_snapshot.has_value()) {
        shape.subgraphs.emplace(
            node.meta.key,
            detail::to_restore_shape(*node.meta.subgraph_snapshot));
      }
    }
    std::sort(shape.nodes.begin(), shape.nodes.end(), detail::restore_node_less);

    shape.edges.reserve(compiled_execution_index_.index.indexed_edges.size());
    for (const auto &edge : compiled_execution_index_.index.indexed_edges) {
      shape.edges.push_back(graph_restore_edge{
          .from = compiled_execution_index_.index.id_to_key[edge.from],
          .to = compiled_execution_index_.index.id_to_key[edge.to],
          .no_control = edge.no_control,
          .no_data = edge.no_data,
          .adapter_kind = edge.adapter.kind,
          .has_custom_value_to_stream =
              edge.adapter.custom.value_to_stream.has_value(),
          .has_custom_stream_to_value =
              edge.adapter.custom.stream_to_value.has_value(),
      });
    }
    std::sort(shape.edges.begin(), shape.edges.end(), detail::restore_edge_less);

    shape.branches.reserve(
        compiled_execution_index_.index.has_branch_by_source.size());
    for (std::uint32_t source_id = 0U;
         source_id < static_cast<std::uint32_t>(
                         compiled_execution_index_.index
                             .has_branch_by_source.size());
         ++source_id) {
      const auto *branch =
          compiled_execution_index_.index.branch_for_source(source_id);
      if (branch == nullptr) {
        continue;
      }
      auto branch_shape = graph_restore_branch{
          .from = compiled_execution_index_.index.id_to_key[source_id],
      };
      branch_shape.end_nodes.reserve(branch->end_nodes_sorted.size());
      for (const auto node_id : branch->end_nodes_sorted) {
        branch_shape.end_nodes.push_back(
            compiled_execution_index_.index.id_to_key[node_id]);
      }
      std::sort(branch_shape.end_nodes.begin(), branch_shape.end_nodes.end());
      shape.branches.push_back(std::move(branch_shape));
    }
    std::sort(shape.branches.begin(), shape.branches.end(),
              detail::restore_branch_less);
    return shape;
  }

  shape.nodes.reserve(nodes_.size());
  for (const auto &node_key : node_insertion_order_) {
    const auto node_iter = nodes_.find(node_key);
    if (node_iter == nodes_.end() || node_key == graph_start_node_key ||
        node_key == graph_end_node_key) {
      continue;
    }
    const auto input_contract = authored_input_contract(node_iter->second);
    const auto &options = authored_options(node_iter->second);
    shape.nodes.push_back(graph_restore_node{
        .key = node_key,
        .kind = authored_kind(node_iter->second),
        .input_contract = input_contract,
        .allow_no_control = options.allow_no_control,
        .allow_no_data = options.allow_no_data,
    });
  }
  std::sort(shape.nodes.begin(), shape.nodes.end(), detail::restore_node_less);

  shape.edges.reserve(edges_.size());
  for (const auto &edge : edges_) {
    shape.edges.push_back(graph_restore_edge{
        .from = edge.from,
        .to = edge.to,
        .no_control = edge.options.no_control,
        .no_data = edge.options.no_data,
        .adapter_kind = edge.options.adapter.kind,
        .has_custom_value_to_stream =
            edge.options.adapter.custom.value_to_stream.has_value(),
        .has_custom_stream_to_value =
            edge.options.adapter.custom.stream_to_value.has_value(),
    });
  }
  std::sort(shape.edges.begin(), shape.edges.end(), detail::restore_edge_less);

  shape.branches.reserve(branches_.size());
  for (const auto &[source, branch] : branches_) {
    auto branch_shape = graph_restore_branch{
        .from = source,
        .end_nodes = branch.end_nodes,
    };
    std::sort(branch_shape.end_nodes.begin(), branch_shape.end_nodes.end());
    shape.branches.push_back(std::move(branch_shape));
  }
  std::sort(shape.branches.begin(), shape.branches.end(),
            detail::restore_branch_less);
  return shape;
}

inline auto graph::rebind_compiled_execution_index_nodes() noexcept -> void {
  compiled_execution_index_.index.nodes_by_id.clear();
  compiled_execution_index_.index.nodes_by_id.reserve(compiled_nodes_.size());
  for (auto &node : compiled_nodes_) {
    compiled_execution_index_.index.nodes_by_id.push_back(&node);
  }
}

inline auto graph::make_csr_offsets(const std::vector<std::uint32_t> &counts)
    -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> offsets(counts.size() + 1U, 0U);
  for (std::size_t index = 0U; index < counts.size(); ++index) {
    offsets[index + 1U] = offsets[index] + counts[index];
  }
  return offsets;
}

inline auto graph::compile_authored(const authored_node &node) -> compiled_node {
  return std::visit(
      [](const auto &value) -> compiled_node {
        return value.compile();
      },
      node);
}

inline auto graph::build_compiled_execution_index() -> wh::core::result<void> {
  compiled_execution_index compiled_index{};
  auto &index = compiled_index.index;
  auto &plan = compiled_index.plan;
  index.key_to_id.reserve(node_insertion_order_.size());
  index.id_to_key.reserve(node_insertion_order_.size());
  index.nodes_by_id.reserve(node_insertion_order_.size());
  compiled_nodes_.clear();
  compiled_nodes_.reserve(node_insertion_order_.size());

  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_insertion_order_.size());
       ++node_id) {
    const auto &key = node_insertion_order_[node_id];
    index.key_to_id.emplace(key, node_id);
    index.id_to_key.push_back(key);
  }

  const auto start_iter = index.key_to_id.find(graph_start_node_key);
  const auto end_iter = index.key_to_id.find(graph_end_node_key);
  if (start_iter == index.key_to_id.end() || end_iter == index.key_to_id.end()) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }
  index.start_id = start_iter->second;
  index.end_id = end_iter->second;

  index.nodes_by_id.resize(index.id_to_key.size(), nullptr);
  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(index.id_to_key.size()); ++node_id) {
    const auto node_iter = nodes_.find(index.id_to_key[node_id]);
    if (node_iter == nodes_.end()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    compiled_nodes_.push_back(compile_authored(node_iter->second));
    index.nodes_by_id[node_id] = &compiled_nodes_.back();
  }

  index.indexed_edges.reserve(edges_.size());
  for (const auto &edge : edges_) {
    const auto from_id_iter = index.key_to_id.find(edge.from);
    const auto to_id_iter = index.key_to_id.find(edge.to);
    if (from_id_iter == index.key_to_id.end() ||
        to_id_iter == index.key_to_id.end()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto adapter = resolve_edge_adapter(edge);
    if (adapter.has_error()) {
      return wh::core::result<void>::failure(adapter.error());
    }
    const auto source_output = authored_output_contract(nodes_.at(edge.from));
    const auto target_input = authored_input_contract(nodes_.at(edge.to));

    index.indexed_edges.push_back(indexed_edge{
        .from = from_id_iter->second,
        .to = to_id_iter->second,
        .source_output = source_output,
        .target_input = target_input,
        .no_control = edge.options.no_control,
        .no_data = edge.options.no_data,
        .adapter = std::move(adapter).value(),
        .limits = edge.options.limits,
    });
  }

  const auto node_count = index.id_to_key.size();
  std::vector<std::uint32_t> incoming_control_counts(node_count, 0U);
  std::vector<std::uint32_t> incoming_data_counts(node_count, 0U);
  std::vector<std::uint32_t> outgoing_data_counts(node_count, 0U);
  std::vector<std::uint32_t> outgoing_control_counts(node_count, 0U);
  std::vector<std::uint32_t> incoming_control_targets{};
  std::vector<std::uint32_t> incoming_data_targets{};
  std::vector<std::uint32_t> outgoing_data_targets{};
  std::vector<std::uint32_t> outgoing_control_targets{};
  incoming_control_targets.reserve(index.indexed_edges.size());
  incoming_data_targets.reserve(index.indexed_edges.size());
  outgoing_data_targets.reserve(index.indexed_edges.size());
  outgoing_control_targets.reserve(index.indexed_edges.size());
  for (const auto &edge : index.indexed_edges) {
    incoming_control_targets.push_back(edge.to);
    incoming_data_targets.push_back(edge.to);
    outgoing_data_targets.push_back(edge.from);
    outgoing_control_targets.push_back(edge.from);
    if (!edge.no_control) {
      ++incoming_control_counts[edge.to];
      ++outgoing_control_counts[edge.from];
    }
    if (!edge.no_data) {
      ++incoming_data_counts[edge.to];
      ++outgoing_data_counts[edge.from];
    }
  }
  index.incoming_control_edges.offsets = make_csr_offsets(incoming_control_counts);
  index.incoming_data_edges.offsets = make_csr_offsets(incoming_data_counts);
  index.outgoing_data_edges.offsets = make_csr_offsets(outgoing_data_counts);
  index.outgoing_control_edges.offsets =
      make_csr_offsets(outgoing_control_counts);
  index.incoming_control_edges.edge_ids.resize(
      index.incoming_control_edges.offsets.back());
  index.incoming_data_edges.edge_ids.resize(
      index.incoming_data_edges.offsets.back());
  index.outgoing_data_edges.edge_ids.resize(
      index.outgoing_data_edges.offsets.back());
  index.outgoing_control_edges.edge_ids.resize(
      index.outgoing_control_edges.offsets.back());

  auto incoming_control_cursor = index.incoming_control_edges.offsets;
  auto incoming_data_cursor = index.incoming_data_edges.offsets;
  auto outgoing_data_cursor = index.outgoing_data_edges.offsets;
  auto outgoing_control_cursor = index.outgoing_control_edges.offsets;
  for (std::uint32_t edge_id = 0U;
       edge_id < static_cast<std::uint32_t>(index.indexed_edges.size());
       ++edge_id) {
    const auto &edge = index.indexed_edges[edge_id];
    if (!edge.no_control) {
      index.incoming_control_edges.edge_ids[incoming_control_cursor[edge.to]++] =
          edge_id;
      index.outgoing_control_edges.edge_ids[outgoing_control_cursor[edge.from]++] =
          edge_id;
    }
    if (!edge.no_data) {
      index.incoming_data_edges.edge_ids[incoming_data_cursor[edge.to]++] =
          edge_id;
      index.outgoing_data_edges.edge_ids[outgoing_data_cursor[edge.from]++] =
          edge_id;
    }
  }

  plan.edges.resize(index.indexed_edges.size(), edge_flow::value_value);
  plan.outputs.resize(node_count);
  plan.inputs.resize(node_count);
  for (std::uint32_t edge_id = 0U;
       edge_id < static_cast<std::uint32_t>(index.indexed_edges.size());
       ++edge_id) {
    const auto &edge = index.indexed_edges[edge_id];
    edge_flow flow = edge_flow::value_value;
    if (edge.source_output == node_contract::value &&
        edge.target_input == node_contract::stream) {
      flow = edge_flow::value_reader;
    } else if (edge.source_output == node_contract::stream &&
               edge.target_input == node_contract::value) {
      flow = edge_flow::reader_value;
    } else if (edge.source_output == node_contract::stream &&
               edge.target_input == node_contract::stream) {
      flow = edge_flow::reader_reader;
    }
    plan.edges[edge_id] = flow;
    if (edge.no_data) {
      continue;
    }
    if (flow == edge_flow::reader_value || flow == edge_flow::reader_reader) {
      plan.outputs[edge.from].reader_edges.push_back(edge_id);
    }
    if (flow == edge_flow::value_reader || flow == edge_flow::reader_reader) {
      plan.inputs[edge.to].reader_edges.push_back(edge_id);
      continue;
    }
    plan.inputs[edge.to].value_edges.push_back(edge_id);
  }

  index.has_branch_by_source.assign(node_count, 0U);
  index.branch_index_by_source.assign(node_count, graph_index::no_branch_index);
  index.branch_defs.reserve(branches_.size());
  for (const auto &[source_key, branch] : branches_) {
    const auto source_id_iter = index.key_to_id.find(source_key);
    if (source_id_iter == index.key_to_id.end()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    const auto source_id = source_id_iter->second;
    indexed_branch_definition indexed{};
    indexed.selector_ids = branch.selector_ids;
    indexed.end_nodes_sorted.reserve(branch.end_nodes.size());
    for (const auto &target_key : branch.end_nodes) {
      const auto target_id_iter = index.key_to_id.find(target_key);
      if (target_id_iter == index.key_to_id.end()) {
        return wh::core::result<void>::failure(wh::core::errc::not_found);
      }
      indexed.end_nodes_sorted.push_back(target_id_iter->second);
    }
    std::sort(indexed.end_nodes_sorted.begin(), indexed.end_nodes_sorted.end());
    indexed.end_nodes_sorted.erase(
        std::unique(indexed.end_nodes_sorted.begin(), indexed.end_nodes_sorted.end()),
        indexed.end_nodes_sorted.end());
    index.has_branch_by_source[source_id] = 1U;
    index.branch_index_by_source[source_id] =
        static_cast<std::uint32_t>(index.branch_defs.size());
    index.branch_defs.push_back(std::move(indexed));
  }

  index.allow_no_control_ids.reserve(compile_order_.size());
  for (const auto &key : compile_order_) {
    const auto iter = index.key_to_id.find(key);
    if (iter == index.key_to_id.end()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    const auto node_id = iter->second;
    if (node_id == index.start_id) {
      continue;
    }
    const auto *node = index.nodes_by_id[node_id];
    if (node != nullptr && node->meta.options.allow_no_control) {
      index.allow_no_control_ids.push_back(node_id);
    }
  }

  std::vector<bool> root_node_seen(node_count, false);
  auto append_root_node = [&](const std::uint32_t node_id) -> void {
    if (node_id == index.start_id || node_id == index.end_id ||
        node_id >= node_count || root_node_seen[node_id]) {
      return;
    }
    root_node_seen[node_id] = true;
    index.root_node_ids.push_back(node_id);
  };
  for (const auto edge_id : index.outgoing_control(index.start_id)) {
    append_root_node(index.indexed_edges[edge_id].to);
  }
  for (const auto node_id : index.allow_no_control_ids) {
    append_root_node(node_id);
  }

  compiled_execution_index_ = std::move(compiled_index);
  return {};
}

inline auto graph::build_control_graph_index() const
    -> wh::core::result<control_graph_index> {
  control_graph_index index{};
  const auto node_count = node_insertion_order_.size();
  index.key_to_id.reserve(node_count);
  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    index.key_to_id.emplace(node_insertion_order_[node_id], node_id);
  }

  const auto start_iter = index.key_to_id.find(graph_start_node_key);
  const auto end_iter = index.key_to_id.find(graph_end_node_key);
  if (start_iter == index.key_to_id.end() || end_iter == index.key_to_id.end()) {
    return wh::core::result<control_graph_index>::failure(
        wh::core::errc::not_found);
  }
  index.start_id = start_iter->second;
  index.end_id = end_iter->second;

  std::vector<std::uint32_t> out_counts(node_count, 0U);
  index.indegree.assign(node_count, 0U);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> control_edges{};
  control_edges.reserve(edges_.size());
  for (const auto &edge : edges_) {
    if (edge.options.no_control) {
      continue;
    }
    const auto from_iter = index.key_to_id.find(edge.from);
    const auto to_iter = index.key_to_id.find(edge.to);
    if (from_iter == index.key_to_id.end() || to_iter == index.key_to_id.end()) {
      return wh::core::result<control_graph_index>::failure(
          wh::core::errc::not_found);
    }
    const auto from = from_iter->second;
    const auto to = to_iter->second;
    control_edges.emplace_back(from, to);
    ++out_counts[from];
    ++index.indegree[to];
  }

  index.control_out_offsets = make_csr_offsets(out_counts);
  index.control_out_nodes.resize(index.control_out_offsets.back());
  auto cursor = index.control_out_offsets;
  for (const auto &[from, to] : control_edges) {
    index.control_out_nodes[cursor[from]++] = to;
  }

  return index;
}

inline auto graph::topo_sort(const control_graph_index &index) const
    -> wh::core::result<std::vector<std::string>> {
  const auto node_count = node_insertion_order_.size();
  auto indegree = index.indegree;

  std::deque<std::uint32_t> ready{};
  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    if (indegree[node_id] == 0U) {
      ready.push_back(node_id);
    }
  }

  std::vector<std::string> order{};
  order.reserve(node_count);
  while (!ready.empty()) {
    const auto current = ready.front();
    ready.pop_front();
    order.push_back(node_insertion_order_[current]);

    for (const auto next : index.out_neighbors(current)) {
      auto &value = indegree[next];
      if (value == 0U) {
        continue;
      }
      --value;
      if (value == 0U) {
        ready.push_back(next);
      }
    }
  }

  if (order.size() != node_count) {
    if (is_cycle_allowed()) {
      return topo_sort_by_scc(index);
    }
    return wh::core::result<std::vector<std::string>>::failure(
        wh::core::errc::contract_violation);
  }
  return order;
}

inline auto graph::topo_sort_by_scc(const control_graph_index &index) const
    -> wh::core::result<std::vector<std::string>> {
  const auto node_count =
      static_cast<std::uint32_t>(index.control_out_offsets.size() - 1U);
  std::vector<int> visit_index(node_count, -1);
  std::vector<int> lowlink(node_count, -1);
  dynamic_bitset on_stack(node_count, false);
  std::vector<std::uint32_t> tarjan_stack{};
  tarjan_stack.reserve(node_count);
  std::vector<std::uint32_t> comp_id(node_count, 0U);
  int next_index = 0;
  std::uint32_t comp_count = 0U;

  struct call_frame {
    std::uint32_t node{0U};
    std::uint32_t next_neighbor{0U};
    bool entered{false};
  };

  std::vector<call_frame> call_stack{};
  call_stack.reserve(node_count);
  for (std::uint32_t root = 0U; root < node_count; ++root) {
    if (visit_index[root] != -1) {
      continue;
    }
    call_stack.push_back(
        call_frame{.node = root, .next_neighbor = 0U, .entered = false});
    while (!call_stack.empty()) {
      auto &frame = call_stack.back();
      if (!frame.entered) {
        visit_index[frame.node] = next_index;
        lowlink[frame.node] = next_index;
        ++next_index;
        tarjan_stack.push_back(frame.node);
        on_stack.set(frame.node);
        frame.entered = true;
      }

      const auto neighbors = index.out_neighbors(frame.node);
      if (frame.next_neighbor < neighbors.size()) {
        const auto next = neighbors[frame.next_neighbor++];
        if (visit_index[next] == -1) {
          call_stack.push_back(
              call_frame{.node = next, .next_neighbor = 0U, .entered = false});
          continue;
        }
        if (on_stack.test(next)) {
          lowlink[frame.node] = std::min(lowlink[frame.node], visit_index[next]);
        }
        continue;
      }

      if (lowlink[frame.node] == visit_index[frame.node]) {
        while (!tarjan_stack.empty()) {
          const auto top = tarjan_stack.back();
          tarjan_stack.pop_back();
          on_stack.clear(top);
          comp_id[top] = comp_count;
          if (top == frame.node) {
            break;
          }
        }
        ++comp_count;
      }

      if (call_stack.size() > 1U) {
        const auto parent = call_stack[call_stack.size() - 2U].node;
        lowlink[parent] = std::min(lowlink[parent], lowlink[frame.node]);
      }
      call_stack.pop_back();
    }
  }

  std::vector<std::pair<std::uint32_t, std::uint32_t>> comp_edges{};
  comp_edges.reserve(index.control_out_nodes.size());
  for (std::uint32_t node_id = 0U; node_id < node_count; ++node_id) {
    for (const auto next : index.out_neighbors(node_id)) {
      const auto from_comp = comp_id[node_id];
      const auto to_comp = comp_id[next];
      if (from_comp != to_comp) {
        comp_edges.emplace_back(from_comp, to_comp);
      }
    }
  }
  std::sort(comp_edges.begin(), comp_edges.end());
  comp_edges.erase(std::unique(comp_edges.begin(), comp_edges.end()),
                   comp_edges.end());

  std::vector<std::vector<std::string>> comp_nodes(comp_count);
  std::vector<std::size_t> comp_first_index(comp_count,
                                            std::numeric_limits<std::size_t>::max());
  for (std::size_t position = 0U; position < node_insertion_order_.size();
       ++position) {
    const auto node_id = index.key_to_id.at(node_insertion_order_[position]);
    const auto group = comp_id[node_id];
    comp_nodes[group].push_back(node_insertion_order_[position]);
    comp_first_index[group] = std::min(comp_first_index[group], position);
  }

  std::vector<std::size_t> comp_indegree(comp_count, 0U);
  std::vector<std::uint32_t> comp_out_counts(comp_count, 0U);
  for (const auto &[from_group, to_group] : comp_edges) {
    ++comp_out_counts[from_group];
    ++comp_indegree[to_group];
  }
  auto comp_out_offsets = make_csr_offsets(comp_out_counts);
  std::vector<std::uint32_t> comp_out_nodes(comp_out_offsets.back(), 0U);
  auto comp_cursor = comp_out_offsets;
  for (const auto &[from_group, to_group] : comp_edges) {
    comp_out_nodes[comp_cursor[from_group]++] = to_group;
  }

  using ready_item = std::pair<std::size_t, std::uint32_t>;
  std::priority_queue<ready_item, std::vector<ready_item>, std::greater<ready_item>>
      ready{};
  for (std::uint32_t group = 0U; group < comp_count; ++group) {
    if (comp_indegree[group] == 0U) {
      ready.emplace(comp_first_index[group], group);
    }
  }

  std::vector<std::string> ordered{};
  ordered.reserve(node_count);
  std::size_t visited_groups = 0U;
  while (!ready.empty()) {
    const auto [group_order, group] = ready.top();
    [[maybe_unused]] const auto ignored_group_order = group_order;
    ready.pop();
    ++visited_groups;
    for (const auto &key : comp_nodes[group]) {
      ordered.push_back(key);
    }
    for (auto offset = comp_out_offsets[group];
         offset < comp_out_offsets[group + 1U]; ++offset) {
      const auto next_group = comp_out_nodes[offset];
      auto &value = comp_indegree[next_group];
      if (value == 0U) {
        continue;
      }
      --value;
      if (value == 0U) {
        ready.emplace(comp_first_index[next_group], next_group);
      }
    }
  }

  if (visited_groups != comp_count || ordered.size() != node_count) {
    return wh::core::result<std::vector<std::string>>::failure(
        wh::core::errc::contract_violation);
  }
  return ordered;
}

inline auto graph::is_reachable_start_to_end(const control_graph_index &index) const
    -> bool {
  std::deque<std::uint32_t> queue{};
  dynamic_bitset visited(node_insertion_order_.size(), false);
  queue.push_back(index.start_id);
  visited.set(index.start_id);

  while (!queue.empty()) {
    const auto current = queue.front();
    queue.pop_front();
    if (current == index.end_id) {
      return true;
    }
    for (const auto next : index.out_neighbors(current)) {
      if (visited.set_if_unset(next)) {
        queue.push_back(next);
      }
    }
  }
  return false;
}

inline auto graph::find_cycle_path(const control_graph_index &index) const
    -> std::optional<std::vector<std::string>> {
  enum class mark : std::uint8_t { white, gray, black };
  const auto node_count =
      static_cast<std::uint32_t>(index.control_out_offsets.size() - 1U);
  std::vector<mark> marks(node_count, mark::white);

  struct dfs_frame {
    std::uint32_t node{0U};
    std::uint32_t next_neighbor{0U};
  };
  std::vector<dfs_frame> dfs_stack{};
  dfs_stack.reserve(node_count);
  std::vector<std::uint32_t> path_stack{};
  path_stack.reserve(node_count);

  for (std::uint32_t root = 0U; root < node_count; ++root) {
    if (marks[root] != mark::white) {
      continue;
    }

    dfs_stack.push_back(dfs_frame{.node = root, .next_neighbor = 0U});
    marks[root] = mark::gray;
    path_stack.push_back(root);

    while (!dfs_stack.empty()) {
      auto &frame = dfs_stack.back();
      const auto neighbors = index.out_neighbors(frame.node);
      if (frame.next_neighbor < neighbors.size()) {
        const auto next = neighbors[frame.next_neighbor++];
        const auto next_mark = marks[next];
        if (next_mark == mark::gray) {
          const auto begin = std::find(path_stack.begin(), path_stack.end(), next);
          std::vector<std::string> cycle{};
          cycle.reserve(static_cast<std::size_t>(path_stack.end() - begin + 1));
          for (auto iter = begin; iter != path_stack.end(); ++iter) {
            cycle.push_back(node_insertion_order_[*iter]);
          }
          cycle.push_back(node_insertion_order_[next]);
          return cycle;
        }
        if (next_mark == mark::white) {
          marks[next] = mark::gray;
          dfs_stack.push_back(dfs_frame{.node = next, .next_neighbor = 0U});
          path_stack.push_back(next);
        }
        continue;
      }

      marks[frame.node] = mark::black;
      dfs_stack.pop_back();
      path_stack.pop_back();
    }
  }
  return std::nullopt;
}

inline auto graph::release_cold_data_after_compile() -> void {
  nodes_.clear();
  nodes_.rehash(0U);
  edges_.clear();
  edges_.shrink_to_fit();
  branches_.clear();
  branches_.rehash(0U);
  node_insertion_order_.clear();
  node_insertion_order_.shrink_to_fit();
  node_id_index_.clear();
  node_id_index_.rehash(0U);
  compile_order_.clear();
  compile_order_.shrink_to_fit();
}

inline constexpr auto graph::is_cycle_allowed() const noexcept -> bool {
  return options_.mode == graph_runtime_mode::pregel;
}

} // namespace wh::compose
