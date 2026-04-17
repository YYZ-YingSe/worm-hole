// Defines compose runtime state/process abstractions used by graph execution,
// checkpoint serialization, and state-handler hooks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/compose/types.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

namespace detail {

template <typename state_t>
[[nodiscard]] inline auto state_type_name() -> std::string_view {
  return wh::core::any_info_v<state_t>.name;
}

template <typename state_t>
[[nodiscard]] inline auto state_not_found_detail() -> std::string {
  return std::string{"state not found, expected="} +
         std::string{state_type_name<state_t>()};
}

template <typename state_t>
[[nodiscard]] inline auto state_type_mismatch_detail(const wh::core::any &value)
    -> std::string {
  return std::string{"state type mismatch, expected="} +
         std::string{state_type_name<state_t>()} +
         ", actual=" + std::string{value.info().name};
}

} // namespace detail

/// Runtime lifecycle state for one graph node execution slot.
enum class graph_node_lifecycle_state : std::uint8_t {
  /// Node has not been considered by runtime scheduler.
  pending = 0U,
  /// Node is currently running.
  running,
  /// Node finished successfully.
  completed,
  /// Node execution failed.
  failed,
  /// Node was skipped by control-flow policy.
  skipped,
  /// Node was canceled/stopped.
  canceled,
};

/// Serializable runtime state for one node.
struct graph_node_state {
  /// Stable node key.
  std::string key{};
  /// Stable node id assigned at compile-time.
  std::uint32_t node_id{0U};
  /// Current lifecycle state.
  graph_node_lifecycle_state lifecycle{graph_node_lifecycle_state::pending};
  /// Retry-attempt index consumed by this node.
  std::size_t attempts{0U};
  /// Optional last error status.
  std::optional<wh::core::error_code> last_error{};
};

/// Causal marker attached to one state transition.
struct graph_state_cause {
  /// Monotonic run-id of current invoke call.
  std::uint64_t run_id{0U};
  /// Scheduler step index inside the run.
  std::size_t step{0U};
  /// Stable node key that caused this transition.
  std::string node_key{};
};

/// Transition kind used by state audit/replay diagnostics.
enum class graph_state_transition_kind : std::uint8_t {
  /// Node entered runtime execution path.
  node_enter = 0U,
  /// Node finished runtime execution path.
  node_leave,
  /// Node was skipped by routing/readiness policy.
  node_skip,
  /// Node failed with an execution error.
  node_fail,
  /// State and route decision were atomically committed.
  route_commit,
};

/// Merge policy used when parallel branch decisions target one logical
/// node-state.
enum class graph_branch_merge : std::uint8_t {
  /// Merge by set-union of selected branch destinations.
  set_union = 0U,
  /// Keep previous decision when a second decision arrives.
  keep_existing,
  /// Replace previous decision with newest decision.
  overwrite,
  /// Reject conflicting decisions and fail fast.
  fail_on_conflict,
};

/// One immutable transition event for replay diagnostics.
struct graph_state_transition_event {
  /// Transition kind recorded by runtime.
  graph_state_transition_kind kind{graph_state_transition_kind::node_enter};
  /// Causal marker of this transition.
  graph_state_cause cause{};
  /// Node lifecycle state after this transition.
  graph_node_lifecycle_state lifecycle{graph_node_lifecycle_state::pending};
};

/// Thread-safe typed process-state store with optional parent chaining.
class graph_process_state {
public:
  graph_process_state() = default;

  explicit graph_process_state(graph_process_state *parent) noexcept
      : parent_(parent) {}

  graph_process_state(const graph_process_state &other) {
    auto lock = std::scoped_lock{other.mutex_};
    values_ = other.values_;
    parent_ = other.parent_;
  }

  auto operator=(const graph_process_state &other) -> graph_process_state & {
    if (this == &other) {
      return *this;
    }
    auto lock = std::scoped_lock{mutex_, other.mutex_};
    values_ = other.values_;
    parent_ = other.parent_;
    return *this;
  }

  graph_process_state(graph_process_state &&other) noexcept {
    auto lock = std::scoped_lock{other.mutex_};
    values_ = std::move(other.values_);
    parent_ = other.parent_;
    other.parent_ = nullptr;
  }

  auto operator=(graph_process_state &&other) noexcept
      -> graph_process_state & {
    if (this == &other) {
      return *this;
    }
    auto lock = std::scoped_lock{mutex_, other.mutex_};
    values_ = std::move(other.values_);
    parent_ = other.parent_;
    other.parent_ = nullptr;
    return *this;
  }

  /// Returns parent process-state used by subgraph fallback lookup.
  [[nodiscard]] auto parent() const noexcept -> graph_process_state * {
    return parent_;
  }

  /// Sets parent process-state used by subgraph fallback lookup.
  auto set_parent(graph_process_state *parent) noexcept -> void {
    parent_ = parent;
  }

  /// Returns thread-local detail of the last typed state access failure.
  [[nodiscard]] static auto last_error_detail() -> std::string_view {
    return last_error_detail_;
  }

  /// Inserts or replaces one typed state object.
  template <typename state_t, typename... args_t>
    requires std::constructible_from<state_t, args_t &&...>
  auto emplace(args_t &&...args)
      -> wh::core::result<std::reference_wrapper<state_t>> {
    auto lock = std::scoped_lock{mutex_};
    auto &slot = values_[wh::core::any_type_key_v<state_t>];
    slot.template emplace<state_t>(std::forward<args_t>(args)...);
    last_error_detail_.clear();
    return std::ref(*wh::core::any_cast<state_t>(&slot));
  }

  /// Returns mutable typed state by recursive parent lookup.
  template <typename state_t>
  [[nodiscard]] auto get()
      -> wh::core::result<std::reference_wrapper<state_t>> {
    {
      auto lock = std::scoped_lock{mutex_};
      auto iter = values_.find(wh::core::any_type_key_v<state_t>);
      if (iter != values_.end()) {
        auto *typed = wh::core::any_cast<state_t>(&iter->second);
        if (typed == nullptr) {
          last_error_detail_ =
              detail::state_type_mismatch_detail<state_t>(iter->second);
          return wh::core::result<std::reference_wrapper<state_t>>::failure(
              wh::core::errc::type_mismatch);
        }
        last_error_detail_.clear();
        return std::ref(*typed);
      }
    }
    if (parent_ != nullptr) {
      return parent_->get<state_t>();
    }
    last_error_detail_ = detail::state_not_found_detail<state_t>();
    return wh::core::result<std::reference_wrapper<state_t>>::failure(
        wh::core::errc::not_found);
  }

  /// Returns immutable typed state by recursive parent lookup.
  template <typename state_t>
  [[nodiscard]] auto get() const
      -> wh::core::result<std::reference_wrapper<const state_t>> {
    {
      auto lock = std::scoped_lock{mutex_};
      auto iter = values_.find(wh::core::any_type_key_v<state_t>);
      if (iter != values_.end()) {
        const auto *typed = wh::core::any_cast<state_t>(&iter->second);
        if (typed == nullptr) {
          last_error_detail_ =
              detail::state_type_mismatch_detail<state_t>(iter->second);
          return wh::core::result<std::reference_wrapper<const state_t>>::
              failure(wh::core::errc::type_mismatch);
        }
        last_error_detail_.clear();
        return std::cref(*typed);
      }
    }
    if (parent_ != nullptr) {
      return parent_->get<state_t>();
    }
    last_error_detail_ = detail::state_not_found_detail<state_t>();
    return wh::core::result<std::reference_wrapper<const state_t>>::failure(
        wh::core::errc::not_found);
  }

private:
  static inline thread_local std::string last_error_detail_{};
  mutable std::mutex mutex_{};
  std::unordered_map<wh::core::any_type_key, wh::core::any,
                     wh::core::any_type_key_hash>
      values_{};
  graph_process_state *parent_{nullptr};
};

/// Node-level state pre-handler (value path).
using graph_state_pre_handler =
    wh::core::callback_function<wh::core::result<void>(
        const graph_state_cause &, graph_process_state &, graph_value &,
        wh::core::run_context &) const>;

/// Node-level state post-handler (value path).
using graph_state_post_handler =
    wh::core::callback_function<wh::core::result<void>(
        const graph_state_cause &, graph_process_state &, graph_value &,
        wh::core::run_context &) const>;

/// Node-level stream pre-handler (chunk path).
using graph_stream_pre = wh::core::callback_function<wh::core::result<void>(
    const graph_state_cause &, graph_process_state &, graph_value &,
    wh::core::run_context &) const>;

/// Node-level stream post-handler (chunk path).
using graph_stream_post = wh::core::callback_function<wh::core::result<void>(
    const graph_state_cause &, graph_process_state &, graph_value &,
    wh::core::run_context &) const>;

/// Aggregated handler set bound to one node key.
struct graph_node_state_handlers {
  /// Optional value pre-handler.
  graph_state_pre_handler pre{nullptr};
  /// Optional value post-handler.
  graph_state_post_handler post{nullptr};
  /// Optional stream pre-handler.
  graph_stream_pre stream_pre{nullptr};
  /// Optional stream post-handler.
  graph_stream_post stream_post{nullptr};
};

/// Runtime handler registry keyed by node key.
using graph_state_handler_registry =
    std::unordered_map<std::string, graph_node_state_handlers,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>;

/// Transition log emitted by one invoke run.
using graph_transition_log = std::vector<graph_state_transition_event>;

/// Mutable node-state table keyed by stable node key/id.
class graph_state_table {
public:
  graph_state_table() = default;

  /// Resets one table from compile-stable node keys ordered by node id.
  auto reset(const std::span<const std::string> keys) -> void {
    keys_ = keys;
    states_.clear();
    states_.resize(keys.size());
  }

  /// Returns immutable state entry by node key.
  [[nodiscard]] auto by_key(const std::string_view key) const
      -> wh::core::result<graph_node_state> {
    const auto node_id = find_node_id(key);
    if (!node_id.has_value()) {
      return wh::core::result<graph_node_state>::failure(
          wh::core::errc::not_found);
    }
    return snapshot(*node_id);
  }

  /// Returns immutable state entry by stable node id.
  [[nodiscard]] auto by_id(const std::uint32_t node_id) const
      -> wh::core::result<graph_node_state> {
    if (node_id >= states_.size()) {
      return wh::core::result<graph_node_state>::failure(
          wh::core::errc::not_found);
    }
    return snapshot(node_id);
  }

  /// Updates lifecycle/attempt/error for one node key.
  auto
  update(const std::string_view key, const graph_node_lifecycle_state lifecycle,
         const std::size_t attempts = 0U,
         const std::optional<wh::core::error_code> last_error = std::nullopt)
      -> wh::core::result<void> {
    const auto node_id = find_node_id(key);
    if (!node_id.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return update(*node_id, lifecycle, attempts, last_error);
  }

  /// Updates lifecycle/attempt/error for one stable node id.
  auto
  update(const std::uint32_t node_id,
         const graph_node_lifecycle_state lifecycle,
         const std::size_t attempts = 0U,
         const std::optional<wh::core::error_code> last_error = std::nullopt)
      -> wh::core::result<void> {
    if (node_id >= states_.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto &state = states_[node_id];
    state.lifecycle = lifecycle;
    state.attempts = attempts;
    state.last_error = last_error;
    return {};
  }

  /// Returns immutable contiguous state view for checkpoint serialization.
  [[nodiscard]] auto states() const -> std::vector<graph_node_state> {
    std::vector<graph_node_state> snapshot_states{};
    snapshot_states.reserve(states_.size());
    for (std::uint32_t node_id = 0U;
         node_id < static_cast<std::uint32_t>(states_.size()); ++node_id) {
      snapshot_states.push_back(snapshot(node_id));
    }
    return snapshot_states;
  }

  /// Collects stable node keys whose lifecycle matches `lifecycle`.
  [[nodiscard]] auto
  collect_keys(const graph_node_lifecycle_state lifecycle) const
      -> std::vector<std::string> {
    std::vector<std::string> keys{};
    keys.reserve(states_.size());
    for (std::uint32_t node_id = 0U;
         node_id < static_cast<std::uint32_t>(states_.size()); ++node_id) {
      if (states_[node_id].lifecycle != lifecycle) {
        continue;
      }
      keys.emplace_back(keys_[node_id]);
    }
    return keys;
  }

  /// Collects stable node keys for completed lifecycle slots.
  [[nodiscard]] auto collect_completed_keys() const
      -> std::vector<std::string> {
    return collect_keys(graph_node_lifecycle_state::completed);
  }

private:
  struct state_slot {
    graph_node_lifecycle_state lifecycle{graph_node_lifecycle_state::pending};
    std::size_t attempts{0U};
    std::optional<wh::core::error_code> last_error{};
  };

  [[nodiscard]] auto find_node_id(const std::string_view key) const noexcept
      -> std::optional<std::uint32_t> {
    for (std::uint32_t node_id = 0U;
         node_id < static_cast<std::uint32_t>(keys_.size()); ++node_id) {
      if (keys_[node_id] == key) {
        return node_id;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto snapshot(const std::uint32_t node_id) const
      -> graph_node_state {
    const auto &state = states_[node_id];
    return graph_node_state{
        .key = std::string{keys_[node_id]},
        .node_id = node_id,
        .lifecycle = state.lifecycle,
        .attempts = state.attempts,
        .last_error = state.last_error,
    };
  }

  std::span<const std::string> keys_{};
  std::vector<state_slot> states_{};
};

} // namespace wh::compose
