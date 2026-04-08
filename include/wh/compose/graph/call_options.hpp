// Defines per-invocation graph call options frozen at run start.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/node/path.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

/// Invoke-scoped trace input frozen at graph entry.
struct graph_trace_context {
  /// Distributed trace id reused by all nodes in this invoke run.
  std::string trace_id{};
  /// Parent span id for the graph root span.
  std::string parent_span_id{};
};

/// Per-call stream channel kind selectable by graph callers.
enum class graph_stream_channel_kind : std::uint8_t {
  /// Full state snapshot stream.
  state_snapshot = 0U,
  /// Incremental state-delta stream.
  state_delta,
  /// Message/business-event stream.
  message,
  /// Custom caller-defined stream channel.
  custom,
  /// Runtime debug-decision stream.
  debug,
};

/// Stream subscription request for one channel kind.
struct graph_stream_subscription {
  /// Stream channel kind requested by caller.
  graph_stream_channel_kind kind{graph_stream_channel_kind::state_snapshot};
  /// Custom channel name when `kind == custom`.
  std::string custom_channel{};
  /// True keeps this subscription active for current run.
  bool enabled{true};
};

/// External interrupt timeout behavior semantics.
enum class graph_interrupt_timeout_mode : std::uint8_t {
  /// No timeout means waiting in-flight work to converge naturally.
  wait_inflight = 0U,
  /// Zero timeout means entering rerun decision path immediately.
  immediate_rerun,
};

/// External interrupt policy frozen on first trigger.
struct graph_external_interrupt_policy {
  /// Optional timeout budget for external interrupt convergence.
  std::optional<std::chrono::milliseconds> timeout{};
  /// Timeout interpretation mode for rerun handoff.
  graph_interrupt_timeout_mode mode{
      graph_interrupt_timeout_mode::wait_inflight};
  /// True enables automatic persist for external interrupts.
  bool auto_persist_external_interrupt{true};
  /// True keeps internal interrupt persist on manual path only.
  bool manual_persist_internal_interrupt{true};
};

/// Debug scheduling decision emitted on isolated debug stream.
struct graph_debug_stream_event {
  /// Scheduler decision type emitted by runtime.
  enum class decision_kind : std::uint8_t {
    /// Node has been queued for execution.
    enqueue = 0U,
    /// Node has been dequeued for readiness check.
    dequeue,
    /// Node has been skipped by runtime policy.
    skipped,
    /// Node has been retried due to retry budget.
    retry,
    /// Interrupt target has been matched.
    interrupt_hit,
  };

  /// Emitted scheduling decision.
  decision_kind decision{decision_kind::enqueue};
  /// Stable node key bound to this decision.
  std::string node_key{};
  /// Full node path when decision happens in nested graph.
  node_path path{};
  /// Runtime step index when decision was recorded.
  std::size_t step{0U};
};

/// Debug callback invoked for scheduler decisions in one run.
using graph_debug_callback = wh::core::callback_function<void(
    const graph_debug_stream_event &, wh::core::run_context &) const>;

/// Node-path scoped debug callback injection entry.
struct graph_node_path_debug_callback {
  /// Target node path for this callback scope.
  node_path path{};
  /// True means descendants under `path` are also matched.
  bool include_descendants{true};
  /// Callback invoked when matched debug event is emitted.
  graph_debug_callback callback{nullptr};
};

/// One node-path component override.
struct graph_component_override {
  /// Target node path; empty path means current graph root.
  node_path path{};
  /// Override payloads keyed by logical option key.
  graph_value_map values{};
};

/// Invoke-scoped node observation override matched by node path.
struct graph_node_observation_override {
  /// Target node path used for exact/subtree matching.
  node_path path{};
  /// True means descendants under `path` are also matched.
  bool include_descendants{false};
  /// Optional callback emission override; absent keeps node default.
  std::optional<bool> callbacks_enabled{};
  /// Optional local callback registrations appended after node defaults.
  std::optional<graph_node_callback_plan> local_callbacks{};
};

/// Runtime-owned resolved observation state for one node.
struct graph_resolved_node_observation {
  /// Effective callback emission policy for this node run.
  bool callbacks_enabled{true};
  /// Effective ordered local callback registrations for this node run.
  graph_node_callback_plan local_callbacks{};
};

/// One resolved component option entry with source scope metadata.
struct graph_component_option {
  /// Resolved option payload.
  graph_value value{};
  /// True when this option came from a path override.
  bool from_override{false};
};

/// Resolved component option map used by typed extraction helpers.
using graph_component_option_map =
    std::unordered_map<std::string, graph_component_option,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>;

/// One run-scoped call options bundle frozen at invoke start.
struct graph_call_options {
  /// Invoke-scoped distributed trace input.
  std::optional<graph_trace_context> trace{};
  /// Ordered node observation override list (`last` scalar match wins).
  std::vector<graph_node_observation_override> node_observations{};
  /// Global component defaults broadcast to all compatible nodes.
  graph_value_map component_defaults{};
  /// Node-path component overrides.
  std::vector<graph_component_override> component_overrides{};
  /// Top-level designated node keys (`DesignateNode` scope only).
  std::vector<std::string> designated_top_level_nodes{};
  /// Full node-path designations for subgraph/internal targeting.
  std::vector<node_path> designated_node_paths{};
  /// Per-call multi-stream subscription requests.
  std::vector<graph_stream_subscription> stream_subscriptions{};
  /// True records the full transition log into run-context session outputs.
  bool record_transition_log{false};
  /// Isolates debug stream from business stream channels when true.
  bool isolate_debug_stream{true};
  /// Runtime Pregel step-budget override for current run only.
  std::optional<std::size_t> pregel_max_steps{};
  /// Runtime interrupt timeout budget for current run only.
  std::optional<std::chrono::milliseconds> interrupt_timeout{};
  /// Runtime external interrupt policy override for current run only.
  std::optional<graph_external_interrupt_policy> external_interrupt_policy{};
  /// Graph-level debug callback observing all scheduler decisions.
  graph_debug_callback graph_debug_observer{nullptr};
  /// Node-path scoped debug callbacks for local observability.
  std::vector<graph_node_path_debug_callback> node_path_debug_observers{};
  /// Typed invoke-time controls for tools-node dispatch.
  std::optional<tools_call_options> tools{};
};

/// Read-only invoke scope view projected from one root call-options bundle.
class graph_call_scope {
public:
  graph_call_scope() noexcept = default;

  explicit graph_call_scope(const graph_call_options &options,
                            node_path prefix = {}) noexcept
      : options_(std::addressof(options)), prefix_(std::move(prefix)) {}

  [[nodiscard]] static auto root(const graph_call_options &options) noexcept
      -> graph_call_scope {
    return graph_call_scope{options};
  }

  [[nodiscard]] auto options() const noexcept -> const graph_call_options & {
    if (options_ != nullptr) {
      return *options_;
    }
    return empty_options();
  }

  [[nodiscard]] auto prefix() const noexcept -> const node_path & {
    return prefix_;
  }

  [[nodiscard]] auto trace() const noexcept
      -> const std::optional<graph_trace_context> & {
    if (trace_override_.has_value()) {
      return trace_override_;
    }
    return options().trace;
  }

  [[nodiscard]] auto component_defaults() const noexcept
      -> const graph_value_map & {
    return options().component_defaults;
  }

  [[nodiscard]] auto record_transition_log() const noexcept -> bool {
    return options().record_transition_log;
  }

  [[nodiscard]] auto isolate_debug_stream() const noexcept -> bool {
    return options().isolate_debug_stream;
  }

  [[nodiscard]] auto pregel_max_steps() const noexcept
      -> const std::optional<std::size_t> & {
    return options().pregel_max_steps;
  }

  [[nodiscard]] auto interrupt_timeout() const noexcept
      -> const std::optional<std::chrono::milliseconds> & {
    return options().interrupt_timeout;
  }

  [[nodiscard]] auto external_interrupt_policy() const noexcept
      -> const std::optional<graph_external_interrupt_policy> & {
    return options().external_interrupt_policy;
  }

  [[nodiscard]] auto graph_debug_observer() const noexcept
      -> const graph_debug_callback & {
    return options().graph_debug_observer;
  }

  [[nodiscard]] auto tools() const noexcept
      -> const std::optional<tools_call_options> & {
    return options().tools;
  }

  [[nodiscard]] auto absolute_path(const node_path &path) const -> node_path;

  [[nodiscard]] auto relative_path(const node_path &path) const
      -> std::optional<node_path>;

  [[nodiscard]] auto with_trace(graph_trace_context trace) const
      -> graph_call_scope {
    auto scoped = *this;
    scoped.trace_override_ = std::move(trace);
    return scoped;
  }

private:
  [[nodiscard]] static auto empty_options() noexcept
      -> const graph_call_options & {
    static const graph_call_options options{};
    return options;
  }

  const graph_call_options *options_{nullptr};
  node_path prefix_{};
  std::optional<graph_trace_context> trace_override_{};
};

/// External interrupt resolution chosen at runtime.
enum class graph_external_interrupt_resolution_kind : std::uint8_t {
  /// Wait in-flight mode resolved without deadline timeout.
  wait_inflight = 0U,
  /// Immediate rerun mode resolved at first interrupt trigger.
  immediate_rerun,
};

namespace detail {

[[nodiscard]] inline auto trim_node_path_prefix(const node_path &path,
                                                const node_path &prefix)
    -> node_path {
  const auto segments = path.segments();
  std::vector<std::string_view> remainder{};
  remainder.reserve(segments.size() - prefix.size());
  for (std::size_t index = prefix.size(); index < segments.size(); ++index) {
    remainder.push_back(segments[index]);
  }
  return make_node_path(
      std::span<const std::string_view>{remainder.data(), remainder.size()});
}

[[nodiscard]] inline auto join_node_path(const node_path &prefix,
                                         const node_path &path) -> node_path {
  if (prefix.empty()) {
    return path;
  }
  if (path.empty()) {
    return prefix;
  }

  std::vector<std::string_view> segments{};
  segments.reserve(prefix.size() + path.size());
  for (const auto &segment : prefix.segments()) {
    segments.push_back(segment);
  }
  for (const auto &segment : path.segments()) {
    segments.push_back(segment);
  }
  return make_node_path(
      std::span<const std::string_view>{segments.data(), segments.size()});
}

} // namespace detail

inline auto graph_call_scope::absolute_path(const node_path &path) const
    -> node_path {
  return detail::join_node_path(prefix_, path);
}

inline auto graph_call_scope::relative_path(const node_path &path) const
    -> std::optional<node_path> {
  if (prefix_.empty()) {
    return path;
  }
  if (!path.starts_with(prefix_)) {
    return std::nullopt;
  }
  return detail::trim_node_path_prefix(path, prefix_);
}

/// One designation match result for top-level/path-targeted options.
struct graph_designation_match {
  /// True when top-level key matches designated top-level targets.
  bool top_level_hit{false};
  /// True when full node path matches designated node-path targets.
  bool node_path_hit{false};

  /// Returns true if either designation scope matches.
  [[nodiscard]] constexpr auto matched() const noexcept -> bool {
    return top_level_hit || node_path_hit;
  }
};

/// One stream event namespace for nested graph attribution.
struct graph_event_scope {
  /// Graph name segment for event source attribution.
  std::string graph{};
  /// Node key segment for event source attribution.
  std::string node{};
  /// Full path segment for subgraph attribution.
  std::string path{};
};

/// Generic state-snapshot event emitted on the state-snapshot stream.
struct graph_state_snapshot_event {
  /// Event namespace (`graph/node/path`).
  graph_event_scope scope{};
  /// Runtime step index.
  std::size_t step{0U};
  /// Snapshot value (implementation-defined shape).
  graph_value snapshot{};
};

/// Generic state-delta event emitted on the state-delta stream.
struct graph_state_delta_event {
  /// Event namespace (`graph/node/path`).
  graph_event_scope scope{};
  /// Runtime step index.
  std::size_t step{0U};
  /// Delta value (implementation-defined shape).
  graph_value delta{};
};

/// Generic runtime-message event emitted on the message stream.
struct graph_runtime_message_event {
  /// Event namespace (`graph/node/path`).
  graph_event_scope scope{};
  /// Runtime step index.
  std::size_t step{0U};
  /// Human-readable runtime text.
  std::string text{};
};

/// Generic custom stream event payload.
struct graph_custom_event {
  /// Event namespace (`graph/node/path`).
  graph_event_scope scope{};
  /// Runtime step index.
  std::size_t step{0U};
  /// Caller-defined custom channel name.
  std::string channel{};
  /// Custom payload (implementation-defined shape).
  graph_value payload{};
};

/// Frozen external interrupt policy holder (first trigger wins).
struct graph_external_interrupt_policy_latch {
  /// True once first external interrupt has frozen the policy.
  bool frozen{false};
  /// Frozen policy snapshot used by subsequent triggers.
  graph_external_interrupt_policy policy{};
};

/// Resolves whether one node hits top-level/path designation scopes.
[[nodiscard]] inline auto
resolve_graph_designation(const graph_call_scope &scope,
                          const std::string_view top_level_node_key,
                          const node_path &path) -> graph_designation_match {
  graph_designation_match match{};
  const auto &options = scope.options();
  if (scope.prefix().empty() && !top_level_node_key.empty()) {
    match.top_level_hit = std::ranges::any_of(
        options.designated_top_level_nodes,
        [&](const std::string &key) { return key == top_level_node_key; });
  }
  const auto absolute = scope.absolute_path(path);
  match.node_path_hit = std::ranges::any_of(
      options.designated_node_paths,
      [&](const node_path &candidate) { return candidate == absolute; });
  return match;
}

/// Resolves whether one node hits top-level/path designation scopes.
[[nodiscard]] inline auto
resolve_graph_designation(const graph_call_options &options,
                          const std::string_view top_level_node_key,
                          const node_path &path) -> graph_designation_match {
  return resolve_graph_designation(graph_call_scope::root(options),
                                   top_level_node_key, path);
}

/// Returns true when any designation scope hits this node.
[[nodiscard]] inline auto
is_graph_node_designated(const graph_call_scope &scope,
                         const std::string_view top_level_node_key,
                         const node_path &path) -> bool {
  return resolve_graph_designation(scope, top_level_node_key, path).matched();
}

/// Returns true when any designation scope hits this node.
[[nodiscard]] inline auto
is_graph_node_designated(const graph_call_options &options,
                         const std::string_view top_level_node_key,
                         const node_path &path) -> bool {
  return resolve_graph_designation(options, top_level_node_key, path).matched();
}

/// Returns true when one stream kind is enabled in call options.
[[nodiscard]] inline auto has_graph_stream_subscription(
    const graph_call_scope &scope, const graph_stream_channel_kind kind,
    const std::string_view custom_channel = {}) -> bool {
  const auto &options = scope.options();
  for (const auto &subscription : options.stream_subscriptions) {
    if (!subscription.enabled || subscription.kind != kind) {
      continue;
    }
    if (kind == graph_stream_channel_kind::custom &&
        subscription.custom_channel != custom_channel) {
      continue;
    }
    return true;
  }
  return false;
}

/// Returns true when one stream kind is enabled in call options.
[[nodiscard]] inline auto has_graph_stream_subscription(
    const graph_call_options &options, const graph_stream_channel_kind kind,
    const std::string_view custom_channel = {}) -> bool {
  for (const auto &subscription : options.stream_subscriptions) {
    if (!subscription.enabled || subscription.kind != kind) {
      continue;
    }
    if (kind == graph_stream_channel_kind::custom &&
        subscription.custom_channel != custom_channel) {
      continue;
    }
    return true;
  }
  return false;
}

/// Returns true when `rule` matches `node` exactly or by subtree prefix.
[[nodiscard]] inline auto
matches_node_observation(const node_path &node,
                         const graph_node_observation_override &rule) -> bool {
  if (rule.path.empty()) {
    return rule.include_descendants;
  }
  if (rule.include_descendants) {
    return node.starts_with(rule.path);
  }
  return node == rule.path;
}

/// Returns true when one observation rule is visible in `scope` and matches
/// `node`.
[[nodiscard]] inline auto
matches_node_observation(const graph_call_scope &scope, const node_path &node,
                         const graph_node_observation_override &rule) -> bool {
  auto relative = scope.relative_path(rule.path);
  if (!relative.has_value()) {
    return false;
  }
  if (relative->empty()) {
    return rule.include_descendants;
  }
  if (rule.include_descendants) {
    return node.starts_with(*relative);
  }
  return node == *relative;
}

/// Returns true when any debug observer is active for the run.
[[nodiscard]] inline auto
has_graph_debug_observer(const graph_call_scope &scope) noexcept -> bool {
  const auto &options = scope.options();
  return static_cast<bool>(options.graph_debug_observer) ||
         !options.node_path_debug_observers.empty();
}

/// Returns true when any debug observer is active for the run.
[[nodiscard]] inline auto
has_graph_debug_observer(const graph_call_options &options) noexcept -> bool {
  return static_cast<bool>(options.graph_debug_observer) ||
         !options.node_path_debug_observers.empty();
}

/// Returns true when runtime must materialize debug events for this run.
[[nodiscard]] inline auto
should_emit_graph_debug_event(const graph_call_scope &scope) -> bool {
  if (has_graph_debug_observer(scope) ||
      has_graph_stream_subscription(scope, graph_stream_channel_kind::debug)) {
    return true;
  }
  return !scope.isolate_debug_stream() &&
         has_graph_stream_subscription(scope,
                                       graph_stream_channel_kind::message);
}

/// Returns true when runtime must materialize debug events for this run.
[[nodiscard]] inline auto
should_emit_graph_debug_event(const graph_call_options &options) -> bool {
  if (has_graph_debug_observer(options) ||
      has_graph_stream_subscription(options,
                                    graph_stream_channel_kind::debug)) {
    return true;
  }
  return !options.isolate_debug_stream &&
         has_graph_stream_subscription(options,
                                       graph_stream_channel_kind::message);
}

/// Builds stable `graph/node/path` namespace for one stream event.
[[nodiscard]] inline auto
make_graph_event_scope(const std::string_view graph_name,
                       const std::string_view node_key, const node_path &path)
    -> graph_event_scope {
  return graph_event_scope{
      .graph = std::string{graph_name},
      .node = std::string{node_key},
      .path = path.to_string(),
  };
}

/// Freezes external interrupt policy once; later calls keep initial policy.
[[nodiscard]] inline auto
freeze_external_interrupt_policy(graph_external_interrupt_policy_latch &latch,
                                 const graph_external_interrupt_policy &policy)
    -> const graph_external_interrupt_policy & {
  if (!latch.frozen) {
    latch.policy = policy;
    latch.frozen = true;
  }
  return latch.policy;
}

/// Resolves external interrupt policy from call options and timeout fallback.
[[nodiscard]] inline auto
resolve_external_interrupt_policy(const graph_call_scope &scope)
    -> graph_external_interrupt_policy {
  const auto &options = scope.options();
  if (options.external_interrupt_policy.has_value()) {
    return *options.external_interrupt_policy;
  }
  graph_external_interrupt_policy resolved{};
  resolved.timeout = options.interrupt_timeout;
  if (resolved.timeout.has_value() &&
      resolved.timeout.value() == std::chrono::milliseconds{0}) {
    resolved.mode = graph_interrupt_timeout_mode::immediate_rerun;
  }
  return resolved;
}

/// Resolves external interrupt policy from call options and timeout fallback.
[[nodiscard]] inline auto
resolve_external_interrupt_policy(const graph_call_options &options)
    -> graph_external_interrupt_policy {
  if (options.external_interrupt_policy.has_value()) {
    return *options.external_interrupt_policy;
  }
  graph_external_interrupt_policy resolved{};
  resolved.timeout = options.interrupt_timeout;
  if (resolved.timeout.has_value() &&
      resolved.timeout.value() == std::chrono::milliseconds{0}) {
    resolved.mode = graph_interrupt_timeout_mode::immediate_rerun;
  }
  return resolved;
}

/// Dispatches graph-level and node-path scoped debug observers for one event.
inline auto
dispatch_graph_debug_observers(const graph_call_scope &scope,
                               const graph_debug_stream_event &event,
                               wh::core::run_context &context) -> void {
  const auto &options = scope.options();
  if (options.graph_debug_observer) {
    options.graph_debug_observer(event, context);
  }
  const auto absolute = scope.absolute_path(event.path);
  for (const auto &observer : options.node_path_debug_observers) {
    if (!observer.callback || observer.path.empty()) {
      continue;
    }
    const auto matched = observer.include_descendants
                             ? absolute.starts_with(observer.path)
                             : absolute == observer.path;
    if (matched) {
      observer.callback(event, context);
    }
  }
}

/// Dispatches graph-level and node-path scoped debug observers for one event.
inline auto
dispatch_graph_debug_observers(const graph_call_options &options,
                               const graph_debug_stream_event &event,
                               wh::core::run_context &context) -> void {
  dispatch_graph_debug_observers(graph_call_scope::root(options), event,
                                 context);
}

/// Resolves component options for `path` using defaults + exact path override.
[[nodiscard]] inline auto
resolve_graph_component_option_map(const graph_call_scope &scope,
                                   const node_path &path)
    -> graph_component_option_map {
  graph_component_option_map resolved{};
  const auto &options = scope.options();
  resolved.reserve(options.component_defaults.size());
  for (const auto &[key, value] : options.component_defaults) {
    resolved.insert_or_assign(
        key, graph_component_option{.value = value, .from_override = false});
  }
  const auto absolute = scope.absolute_path(path);
  for (const auto &targeted : options.component_overrides) {
    if (targeted.path != absolute) {
      continue;
    }
    for (const auto &[key, value] : targeted.values) {
      resolved.insert_or_assign(
          key, graph_component_option{.value = value, .from_override = true});
    }
  }
  return resolved;
}

/// Resolves component options for `path` using defaults + exact path override.
[[nodiscard]] inline auto
resolve_graph_component_option_map(const graph_call_options &options,
                                   const node_path &path)
    -> graph_component_option_map {
  return resolve_graph_component_option_map(graph_call_scope::root(options),
                                            path);
}

/// Resolves component values for `path` using defaults + exact path override.
[[nodiscard]] inline auto
resolve_graph_component_values(const graph_call_scope &scope,
                               const node_path &path) -> graph_value_map {
  graph_value_map resolved{};
  auto detailed = resolve_graph_component_option_map(scope, path);
  resolved.reserve(detailed.size());
  for (auto &[key, entry] : detailed) {
    resolved.insert_or_assign(std::move(key), std::move(entry.value));
  }
  return resolved;
}

/// Resolves component values for `path` using defaults + exact path override.
[[nodiscard]] inline auto
resolve_graph_component_values(const graph_call_options &options,
                               const node_path &path) -> graph_value_map {
  return resolve_graph_component_values(graph_call_scope::root(options), path);
}

/// Resolves component values for `path` using movable call-options overlay.
[[nodiscard]] inline auto
resolve_graph_component_values(graph_call_options &&options,
                               const node_path &path) -> graph_value_map {
  graph_value_map resolved = std::move(options.component_defaults);
  for (auto &targeted : options.component_overrides) {
    if (targeted.path != path) {
      continue;
    }
    for (auto &[key, value] : targeted.values) {
      resolved.insert_or_assign(std::move(key), std::move(value));
    }
  }
  return resolved;
}

/// Extracts one typed component option from resolved map with scope-aware
/// typing rules.
template <typename option_t>
[[nodiscard]] inline auto
extract_graph_component_option(const graph_component_option_map &resolved,
                               const std::string_view option_key)
    -> wh::core::result<std::optional<option_t>> {
  const auto iter = resolved.find(option_key);
  if (iter == resolved.end()) {
    return std::optional<option_t>{};
  }

  if (const auto *typed = wh::core::any_cast<option_t>(&iter->second.value);
      typed != nullptr) {
    return std::optional<option_t>{*typed};
  }
  if (iter->second.from_override) {
    return wh::core::result<std::optional<option_t>>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::optional<option_t>{};
}

/// Rewrites targeted options by trimming `prefix` and dropping out-of-scope
/// entries.

} // namespace wh::compose
