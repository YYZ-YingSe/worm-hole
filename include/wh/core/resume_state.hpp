// Defines checkpoint and resume-state data structures used to persist and
// restore interrupted execution across graph nodes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/core/address.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::core {

namespace detail {

/// Transparent hash alias for heterogeneous `std::string` lookups.
using transparent_string_hash = wh::core::transparent_string_hash;

/// Transparent equality alias for heterogeneous key lookups.
using transparent_string_equal = wh::core::transparent_string_equal;

/// Prefix range index for subtree operations by materialized path.
class path_prefix_index {
public:
  path_prefix_index() = default;

  /// Adds one `(path, interrupt_id)` mapping.
  /// If the interrupt id already exists, the old path mapping is replaced.
  auto insert(const std::string_view key, const std::string_view interrupt_id) -> void {
    const auto existing = index_by_id_.find(interrupt_id);
    if (existing != index_by_id_.end()) {
      index_.erase(existing->second);
      index_by_id_.erase(existing);
    }

    auto iter = index_.emplace(std::string{key}, std::string{interrupt_id});
    index_by_id_.emplace(iter->second, iter);
  }

  /// Removes the mapping for the given interrupt_id.
  auto erase(const std::string_view interrupt_id) -> void {
    const auto id_iter = index_by_id_.find(interrupt_id);
    if (id_iter == index_by_id_.end()) {
      return;
    }

    index_.erase(id_iter->second);
    index_by_id_.erase(id_iter);
  }

  /// Iterates all interrupt ids under a path prefix.
  template <typename callback_t>
  auto for_each_prefix(const std::string_view prefix, callback_t &&callback) const -> void {
    const auto begin = index_.lower_bound(prefix);
    if (begin == index_.end()) {
      return;
    }

    const auto upper_key = next_lexicographic_key(prefix);
    const auto end = upper_key.has_value() ? index_.lower_bound(*upper_key) : index_.end();
    for (auto iter = begin; iter != end; ++iter) {
      callback(std::string_view{iter->second});
    }
  }

  /// Clears all mappings.
  auto clear() noexcept -> void {
    index_.clear();
    index_by_id_.clear();
  }

private:
  /// Computes upper-bound key used by prefix range scans.
  [[nodiscard]] static auto next_lexicographic_key(std::string_view prefix)
      -> std::optional<std::string> {
    std::string key{prefix};
    for (std::size_t index = key.size(); index > 0U; --index) {
      auto &ch = key[index - 1U];
      if (ch == static_cast<char>(0xFF)) {
        continue;
      }
      ch = static_cast<char>(static_cast<unsigned char>(ch) + 1U);
      key.resize(index);
      return key;
    }
    return std::nullopt;
  }

  using index_map = std::multimap<std::string, std::string, std::less<>>;
  using index_iterator = index_map::iterator;

  index_map index_{};
  std::unordered_map<std::string, index_iterator, transparent_string_hash, transparent_string_equal>
      index_by_id_{};
};

} // namespace detail

/// Snapshot of an emitted interrupt signal.
struct interrupt_signal {
  /// Stable interrupt identifier for lookup and deduplication.
  std::string interrupt_id{};
  /// Runtime address where this interrupt was emitted.
  address location{};
  /// Interrupt-local serialized state payload.
  wh::core::any state{};
  /// Layer-specific payload captured alongside interrupt state.
  wh::core::any layer_payload{};
  /// True once this interrupt has been consumed during resume.
  bool used{false};
  /// Parent address chain from root to direct parent for composite attribution.
  std::vector<address> parent_locations{};
  /// Human-readable trigger reason captured at interrupt emission.
  std::string trigger_reason{};
};

/// Resume-time interrupt context persisted in runtime state.
struct interrupt_context {
  /// Stable interrupt identifier for lookup and deduplication.
  std::string interrupt_id{};
  /// Runtime address where this interrupt context applies.
  address location{};
  /// Resume-local serialized state payload.
  wh::core::any state{};
  /// Layer-specific payload captured for resume processing.
  wh::core::any layer_payload{};
  /// True once this resume context has been consumed.
  bool used{false};
  /// Parent address chain from root to direct parent for composite attribution.
  std::vector<address> parent_locations{};
  /// Human-readable trigger reason captured at interrupt emission.
  std::string trigger_reason{};
};

/// Tree view for runtime interrupt signals.
/// Used by rebuild/flatten/bridge helpers when callers need hierarchical
/// traversal by address subtree instead of flat id lookup.
struct interrupt_signal_tree_node {
  /// Address represented by this tree node.
  address location{};
  /// Signals directly attached to this address node.
  std::vector<interrupt_signal> signals{};
  /// Child address nodes nested under `location`.
  std::vector<interrupt_signal_tree_node> children{};
};

/// Tree view for interrupt contexts.
/// Mirrors interrupt_signal_tree_node so signal/context forms can round-trip
/// without losing address hierarchy.
struct interrupt_context_tree_node {
  /// Address represented by this tree node.
  address location{};
  /// Resume contexts directly attached to this address node.
  std::vector<interrupt_context> contexts{};
  /// Child address nodes nested under `location`.
  std::vector<interrupt_context_tree_node> children{};
};

/// Deep-copies one interrupt payload object for external/exported snapshots.
[[nodiscard]] inline auto clone_interrupt_payload_any(const wh::core::any &payload)
    -> wh::core::any {
  auto owned = wh::core::into_owned(payload);
  if (owned.has_error()) {
    return {};
  }
  return std::move(owned).value();
}

/// Converts signal view to context view (copy).
[[nodiscard]] inline auto to_interrupt_context(const interrupt_signal &signal)
    -> interrupt_context {
  return interrupt_context{signal.interrupt_id,
                           signal.location,
                           clone_interrupt_payload_any(signal.state),
                           clone_interrupt_payload_any(signal.layer_payload),
                           signal.used,
                           signal.parent_locations,
                           signal.trigger_reason};
}

/// Converts signal view to context view (move).
[[nodiscard]] inline auto to_interrupt_context(interrupt_signal &&signal) -> interrupt_context {
  return interrupt_context{std::move(signal.interrupt_id),
                           std::move(signal.location),
                           std::move(signal.state),
                           std::move(signal.layer_payload),
                           signal.used,
                           std::move(signal.parent_locations),
                           std::move(signal.trigger_reason)};
}

/// Converts context view to signal view (copy).
[[nodiscard]] inline auto to_interrupt_signal(const interrupt_context &context)
    -> interrupt_signal {
  return interrupt_signal{context.interrupt_id,
                          context.location,
                          clone_interrupt_payload_any(context.state),
                          clone_interrupt_payload_any(context.layer_payload),
                          context.used,
                          context.parent_locations,
                          context.trigger_reason};
}

/// Converts context view to signal view (move).
[[nodiscard]] inline auto to_interrupt_signal(interrupt_context &&context) -> interrupt_signal {
  return interrupt_signal{std::move(context.interrupt_id),
                          std::move(context.location),
                          std::move(context.state),
                          std::move(context.layer_payload),
                          context.used,
                          std::move(context.parent_locations),
                          std::move(context.trigger_reason)};
}

namespace detail {

/// Finds child node by exact address location.
template <typename node_t>
auto find_child_by_location(std::vector<node_t> &children, const address &location) -> node_t * {
  const auto iter = std::ranges::find_if(
      children, [&](const node_t &node) -> bool { return node.location == location; });
  if (iter == children.end()) {
    return nullptr;
  }
  return std::addressof(*iter);
}

/// Ensures all address segments exist in tree and returns leaf.
template <typename node_t>
auto ensure_tree_path(std::vector<node_t> &roots, const address &location) -> node_t * {
  auto *level = &roots;
  node_t *current_node = nullptr;
  address current_location{};
  for (const auto &segment : location.segments()) {
    current_location = current_location.append(segment);
    auto *child = find_child_by_location(*level, current_location);
    if (child == nullptr) {
      level->push_back(node_t{current_location, {}, {}});
      child = std::addressof(level->back());
    }
    current_node = child;
    level = &current_node->children;
  }

  if (current_node == nullptr) {
    auto *root = find_child_by_location(roots, location);
    if (root != nullptr) {
      return root;
    }
    roots.push_back(node_t{location, {}, {}});
    return std::addressof(roots.back());
  }
  return current_node;
}

/// Appends segment to target only when allowed by filter.
inline auto append_filtered_segment(address &target, const std::string_view segment,
                                    const std::span<const std::string_view> allowed_segments)
    -> void {
  if (allowed_segments.empty()) {
    target = target.append(segment);
    return;
  }
  const auto iter = std::ranges::find(allowed_segments, segment);
  if (iter != allowed_segments.end()) {
    target = target.append(segment);
  }
}

} // namespace detail

/// Projects address by keeping only allowed segments.
[[nodiscard]] inline auto project_address(const address &source,
                                          const std::span<const std::string_view> allowed_segments)
    -> address {
  if (allowed_segments.empty()) {
    return source;
  }

  address projected{};
  for (const auto &segment : source.segments()) {
    detail::append_filtered_segment(projected, segment, allowed_segments);
  }
  return projected;
}

/// Projects interrupt context location by segment filter (copy overload).
[[nodiscard]] inline auto
project_interrupt_context(const interrupt_context &context,
                          const std::span<const std::string_view> allowed_segments)
    -> interrupt_context {
  if (allowed_segments.empty()) {
    return context;
  }
  interrupt_context projected{context};
  projected.location = project_address(context.location, allowed_segments);
  return projected;
}

/// Projects interrupt context location by segment filter (move overload).
[[nodiscard]] inline auto
project_interrupt_context(interrupt_context &&context,
                          const std::span<const std::string_view> allowed_segments)
    -> interrupt_context {
  if (allowed_segments.empty()) {
    return context;
  }
  context.location = project_address(context.location, allowed_segments);
  return context;
}

/// Rebuilds signal tree view from flat signal list.
[[nodiscard]] inline auto
rebuild_interrupt_signal_tree(const std::span<const interrupt_signal> signals)
    -> std::vector<interrupt_signal_tree_node> {
  std::vector<interrupt_signal_tree_node> roots{};
  roots.reserve(signals.size());
  for (const auto &signal : signals) {
    auto *leaf = detail::ensure_tree_path(roots, signal.location);
    leaf->signals.push_back(signal);
  }
  return roots;
}

/// Rebuilds context tree view from flat context list.
[[nodiscard]] inline auto
rebuild_interrupt_context_tree(const std::span<const interrupt_context> contexts)
    -> std::vector<interrupt_context_tree_node> {
  std::vector<interrupt_context_tree_node> roots{};
  roots.reserve(contexts.size());
  for (const auto &context : contexts) {
    auto *leaf = detail::ensure_tree_path(roots, context.location);
    leaf->contexts.push_back(context);
  }
  return roots;
}

namespace detail {

/// Converts signal-tree node to context-tree node recursively.
inline auto to_context_tree_node(const interrupt_signal_tree_node &source)
    -> interrupt_context_tree_node {
  interrupt_context_tree_node node{};
  node.location = source.location;
  node.contexts.reserve(source.signals.size());
  for (const auto &signal : source.signals) {
    node.contexts.push_back(to_interrupt_context(signal));
  }
  node.children.reserve(source.children.size());
  for (const auto &child : source.children) {
    node.children.push_back(to_context_tree_node(child));
  }
  return node;
}

/// Converts context-tree node to signal-tree node recursively.
inline auto to_signal_tree_node(const interrupt_context_tree_node &source)
    -> interrupt_signal_tree_node {
  interrupt_signal_tree_node node{};
  node.location = source.location;
  node.signals.reserve(source.contexts.size());
  for (const auto &context : source.contexts) {
    node.signals.push_back(to_interrupt_signal(context));
  }
  node.children.reserve(source.children.size());
  for (const auto &child : source.children) {
    node.children.push_back(to_signal_tree_node(child));
  }
  return node;
}

/// Flattens one signal-tree node recursively into output vector.
inline auto flatten_signal_tree_node(const interrupt_signal_tree_node &node,
                                     std::vector<interrupt_signal> &output) -> void {
  output.insert(output.end(), node.signals.begin(), node.signals.end());
  for (const auto &child : node.children) {
    flatten_signal_tree_node(child, output);
  }
}

/// Flattens one context-tree node recursively into output vector.
inline auto flatten_context_tree_node(const interrupt_context_tree_node &node,
                                      std::vector<interrupt_context> &output) -> void {
  output.insert(output.end(), node.contexts.begin(), node.contexts.end());
  for (const auto &child : node.children) {
    flatten_context_tree_node(child, output);
  }
}

} // namespace detail

/// Converts signal-tree roots to context-tree roots.
[[nodiscard]] inline auto
to_interrupt_context_tree(const std::span<const interrupt_signal_tree_node> roots)
    -> std::vector<interrupt_context_tree_node> {
  std::vector<interrupt_context_tree_node> converted{};
  converted.reserve(roots.size());
  for (const auto &root : roots) {
    converted.push_back(detail::to_context_tree_node(root));
  }
  return converted;
}

/// Converts context-tree roots to signal-tree roots.
[[nodiscard]] inline auto
to_interrupt_signal_tree(const std::span<const interrupt_context_tree_node> roots)
    -> std::vector<interrupt_signal_tree_node> {
  std::vector<interrupt_signal_tree_node> converted{};
  converted.reserve(roots.size());
  for (const auto &root : roots) {
    converted.push_back(detail::to_signal_tree_node(root));
  }
  return converted;
}

/// Flattens signal-tree roots to flat signal list.
[[nodiscard]] inline auto
flatten_interrupt_signal_tree(const std::span<const interrupt_signal_tree_node> roots)
    -> std::vector<interrupt_signal> {
  std::vector<interrupt_signal> flattened{};
  for (const auto &root : roots) {
    detail::flatten_signal_tree_node(root, flattened);
  }
  return flattened;
}

/// Flattens context-tree roots to flat context list.
[[nodiscard]] inline auto
flatten_interrupt_context_tree(const std::span<const interrupt_context_tree_node> roots)
    -> std::vector<interrupt_context> {
  std::vector<interrupt_context> flattened{};
  for (const auto &root : roots) {
    detail::flatten_context_tree_node(root, flattened);
  }
  return flattened;
}

/// Flattened lookup snapshot built from interrupt signals.
/// It accelerates resume lookup by `interrupt_id`.
struct interrupt_snapshot {
  using address_map = std::unordered_map<std::string, address, detail::transparent_string_hash,
                                         detail::transparent_string_equal>;
  using state_map = std::unordered_map<std::string, wh::core::any, detail::transparent_string_hash,
                                       detail::transparent_string_equal>;

  /// Maps interrupt id to its capture location in the workflow address space.
  address_map interrupt_id_to_address{};
  /// Maps interrupt id to captured state payload for resume.
  state_map interrupt_id_to_state{};
};

/// Builds interrupt snapshot from flat signal list (copy path).
[[nodiscard]] inline auto flatten_interrupt_signals(const std::span<const interrupt_signal> signals)
    -> wh::core::result<interrupt_snapshot> {
  interrupt_snapshot snapshot{};
  snapshot.interrupt_id_to_address.reserve(signals.size());
  snapshot.interrupt_id_to_state.reserve(signals.size());

  for (const auto &signal : signals) {
    auto owned_state = wh::core::into_owned(signal.state);
    if (owned_state.has_error()) {
      return wh::core::result<interrupt_snapshot>::failure(owned_state.error());
    }
    snapshot.interrupt_id_to_address.insert_or_assign(signal.interrupt_id, signal.location);
    snapshot.interrupt_id_to_state.insert_or_assign(signal.interrupt_id,
                                                    std::move(owned_state).value());
  }
  return snapshot;
}

/// Builds interrupt snapshot from flat signal list (move path).
[[nodiscard]] inline auto flatten_interrupt_signals(std::vector<interrupt_signal> &&signals)
    -> wh::core::result<interrupt_snapshot> {
  interrupt_snapshot snapshot{};
  snapshot.interrupt_id_to_address.reserve(signals.size());
  snapshot.interrupt_id_to_state.reserve(signals.size());

  for (auto &signal : signals) {
    auto owned_state = wh::core::into_owned(std::move(signal.state));
    if (owned_state.has_error()) {
      return wh::core::result<interrupt_snapshot>::failure(owned_state.error());
    }
    snapshot.interrupt_id_to_address.insert_or_assign(signal.interrupt_id,
                                                      std::move(signal.location));
    snapshot.interrupt_id_to_state.insert_or_assign(signal.interrupt_id,
                                                    std::move(owned_state).value());
  }
  return snapshot;
}

/// Optional controls for subtree interrupt id collection.
struct resume_subtree_query_options {
  /// Includes already-consumed entries when set.
  bool include_used{false};
};

/// Optional controls for subtree erase operations.
struct resume_subtree_erase_options {
  /// Erases consumed entries together with active entries when set.
  bool include_used{true};
};

/// Mutable state store for pending resume points and their payloads.
class resume_state {
  template <typename value_t> friend struct any_owned_traits;

public:
  /// One resumable entry keyed by `interrupt_id`.
  struct resume_entry {
    /// Logical workflow location used by subtree/target matching.
    address location{};
    /// Opaque payload restored when this entry is consumed.
    wh::core::any data{};
    /// Cached materialized key used by prefix-index operations.
    std::string materialized_path{};
    /// Marks whether this entry was already consumed.
    bool used{false};
  };

  template <typename interrupt_id_t, typename location_t, typename value_t>
    requires std::constructible_from<std::string, interrupt_id_t &&> &&
             std::constructible_from<address, location_t &&> &&
             (!std::same_as<remove_cvref_t<value_t>, wh::core::any>)
  /// Inserts or replaces one resume entry with typed payload.
  auto upsert(interrupt_id_t &&interrupt_id, location_t &&location, value_t &&data)
      -> result<void> {
    using stored_t = remove_cvref_t<value_t>;
    return upsert(std::forward<interrupt_id_t>(interrupt_id), std::forward<location_t>(location),
                  wh::core::any{std::in_place_type<stored_t>, std::forward<value_t>(data)});
  }

  template <typename interrupt_id_t, typename location_t, typename any_t>
    requires std::constructible_from<std::string, interrupt_id_t &&> &&
             std::constructible_from<address, location_t &&> &&
             std::same_as<remove_cvref_t<any_t>, wh::core::any>
  /// Inserts or replaces one resume entry using pre-built `wh::core::any`.
  auto upsert(interrupt_id_t &&interrupt_id, location_t &&location, any_t &&data) -> result<void> {
    std::string stored_interrupt_id{std::forward<interrupt_id_t>(interrupt_id)};
    address stored_location{std::forward<location_t>(location)};
    wh::core::any stored_data{std::forward<any_t>(data)};
    if (stored_interrupt_id.empty()) {
      return result<void>::failure(errc::invalid_argument);
    }

    auto owned_data = wh::core::into_owned(std::move(stored_data));
    if (owned_data.has_error()) {
      return result<void>::failure(owned_data.error());
    }
    stored_data = std::move(owned_data).value();

    const auto iter = entries_.find(stored_interrupt_id);
    if (iter != entries_.end()) {
      if (!iter->second.used) {
        remove_active_location(iter->second.location);
      }
      unindex_location(interrupt_id);
    }

    auto materialized_path = materialize_location_key(stored_location);

    auto updated =
        entries_
            .insert_or_assign(std::move(stored_interrupt_id),
                              resume_entry{std::move(stored_location), std::move(stored_data),
                                           std::move(materialized_path), false})
            .first;
    add_active_location(updated->second.location);
    index_location(updated->first, updated->second.materialized_path);
    return {};
  }

  /// Merges another resume-state snapshot (copy path).
  auto merge(const resume_state &other) -> result<void> {
    if (this == &other) {
      return {};
    }
    auto prepared = wh::core::into_owned(other);
    if (prepared.has_error()) {
      return result<void>::failure(prepared.error());
    }
    return merge(std::move(prepared).value());
  }

  /// Merges another resume-state snapshot (move path).
  auto merge(resume_state &&other) -> result<void> {
    if (this == &other) {
      return {};
    }
    auto prepared = wh::core::into_owned(std::move(other));
    if (prepared.has_error()) {
      return result<void>::failure(prepared.error());
    }

    auto owned = std::move(prepared).value();
    entries_.reserve(entries_.size() + owned.entries_.size());
    for (auto iter = owned.entries_.begin(); iter != owned.entries_.end();) {
      auto node = owned.entries_.extract(iter++);
      const auto current = entries_.find(node.key());
      if (current != entries_.end()) {
        if (!current->second.used) {
          remove_active_location(current->second.location);
        }
        unindex_location(node.key());
        entries_.erase(current);
      }

      if (node.mapped().materialized_path.empty()) {
        node.mapped().materialized_path = materialize_location_key(node.mapped().location);
      }
      auto inserted = entries_.insert(std::move(node));
      index_location(inserted.position->first, inserted.position->second.materialized_path);
      if (!inserted.position->second.used) {
        add_active_location(inserted.position->second.location);
      }
    }

    return {};
  }

  /// Returns whether interrupt id exists.
  [[nodiscard]] auto contains_interrupt_id(const std::string_view interrupt_id) const noexcept
      -> bool {
    return entries_.contains(interrupt_id);
  }

  /// Returns all interrupt ids currently stored in this state.
  [[nodiscard]] auto interrupt_ids(const bool include_used = true) const
      -> std::vector<std::string> {
    std::vector<std::string> ids{};
    ids.reserve(entries_.size());
    for (const auto &[interrupt_id, entry] : entries_) {
      if (!include_used && entry.used) {
        continue;
      }
      ids.push_back(interrupt_id);
    }
    std::ranges::sort(ids);
    return ids;
  }

  /// Returns immutable location for one interrupt id.
  [[nodiscard]] auto location_of(const std::string_view interrupt_id) const
      -> result<std::reference_wrapper<const address>> {
    const auto iter = entries_.find(interrupt_id);
    if (iter == entries_.end()) {
      return result<std::reference_wrapper<const address>>::failure(errc::not_found);
    }
    return std::cref(iter->second.location);
  }

  /// Returns whether state has no entries.
  [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }

  /// Returns number of entries.
  [[nodiscard]] auto size() const noexcept -> std::size_t { return entries_.size(); }

  /// Returns whether location is a valid resume target prefix.
  [[nodiscard]] auto is_resume_target(const address &location) const noexcept -> bool {
    if (active_entry_count_ == 0U) {
      return false;
    }
    if (location.empty()) {
      return true;
    }

    return active_prefix_counts_.contains(location.to_string());
  }

  /// Returns whether location exactly matches an active resume point.
  [[nodiscard]] auto is_exact_resume_target(const address &location) const noexcept -> bool {
    const auto iter = active_exact_counts_.find(location.to_string());
    return iter != active_exact_counts_.end() && iter->second > 0U;
  }

  /// Returns next child segment names under the current resume location.
  [[nodiscard]] auto next_resume_points(const address &location) const -> std::vector<std::string> {
    const auto parent_depth = location.size();
    std::vector<std::string> child_points{};
    child_points.reserve(entries_.size());
    auto child_point_view = entries_ | std::views::values |
                            std::views::filter([&](const resume_entry &entry) {
                              return !entry.used && entry.location.starts_with(location) &&
                                     entry.location.size() > parent_depth;
                            }) |
                            std::views::transform([&](const resume_entry &entry) -> std::string {
                              return std::string{entry.location.segments()[parent_depth]};
                            });
    std::ranges::copy(child_point_view, std::back_inserter(child_points));
    std::ranges::sort(child_points);
    const auto duplicate_begin = std::ranges::unique(child_points).begin();
    child_points.erase(duplicate_begin, child_points.end());

    return child_points;
  }

  /// Collects interrupt ids under a location subtree.
  [[nodiscard]] auto collect_subtree_interrupt_ids(
      const address &location,
      const resume_subtree_query_options options = resume_subtree_query_options{}) const
      -> std::vector<std::string> {
    std::vector<std::string> interrupt_ids{};
    const auto prefix = materialize_location_key(location);
    path_index_.for_each_prefix(prefix, [&](const std::string_view interrupt_id) {
      const auto entry_iter = entries_.find(interrupt_id);
      if (entry_iter == entries_.end()) {
        return;
      }
      if (!options.include_used && entry_iter->second.used) {
        return;
      }
      interrupt_ids.emplace_back(interrupt_id);
    });
    std::ranges::sort(interrupt_ids);
    return interrupt_ids;
  }

  /// Marks all entries under subtree as used.
  auto mark_subtree_used(const address &location) -> std::size_t {
    std::size_t marked_count = 0U;
    const auto prefix = materialize_location_key(location);
    path_index_.for_each_prefix(prefix, [&](const std::string_view interrupt_id) {
      const auto entry_iter = entries_.find(interrupt_id);
      if (entry_iter == entries_.end() || entry_iter->second.used) {
        return;
      }
      remove_active_location(entry_iter->second.location);
      entry_iter->second.used = true;
      ++marked_count;
    });
    return marked_count;
  }

  /// Erases all entries under subtree.
  auto erase_subtree(const address &location,
                     const resume_subtree_erase_options options = resume_subtree_erase_options{})
      -> std::size_t {
    std::size_t removed_count = 0U;
    std::vector<std::string> matched_interrupt_ids{};
    const auto prefix = materialize_location_key(location);
    path_index_.for_each_prefix(prefix, [&](const std::string_view interrupt_id) {
      matched_interrupt_ids.emplace_back(interrupt_id);
    });

    for (const auto &interrupt_id : matched_interrupt_ids) {
      const auto entry_iter = entries_.find(interrupt_id);
      if (entry_iter == entries_.end()) {
        continue;
      }
      if (!options.include_used && entry_iter->second.used) {
        continue;
      }
      if (!entry_iter->second.used) {
        remove_active_location(entry_iter->second.location);
      }
      unindex_location(interrupt_id);
      entries_.erase(entry_iter);
      ++removed_count;
    }
    return removed_count;
  }

  /// Consumes typed payload by interrupt id and marks entry as used.
  template <typename value_t>
  [[nodiscard]] auto consume(const std::string_view interrupt_id) -> result<value_t> {
    const auto iter = entries_.find(interrupt_id);
    if (iter == entries_.end()) {
      return result<value_t>::failure(errc::not_found);
    }
    if (iter->second.used) {
      return result<value_t>::failure(errc::contract_violation);
    }

    auto *typed = wh::core::any_cast<value_t>(&iter->second.data);
    if (typed == nullptr) {
      return result<value_t>::failure(errc::type_mismatch);
    }

    value_t moved = std::move(*typed);
    remove_active_location(iter->second.location);
    iter->second.used = true;
    return moved;
  }

  /// Reads typed payload by interrupt id without consuming/marking-used.
  template <typename value_t>
  [[nodiscard]] auto peek(const std::string_view interrupt_id) const
      -> result<std::reference_wrapper<const value_t>> {
    const auto iter = entries_.find(interrupt_id);
    if (iter == entries_.end()) {
      return result<std::reference_wrapper<const value_t>>::failure(errc::not_found);
    }
    const auto *typed = wh::core::any_cast<value_t>(&iter->second.data);
    if (typed == nullptr) {
      return result<std::reference_wrapper<const value_t>>::failure(errc::type_mismatch);
    }
    return std::cref(*typed);
  }

  /// Marks one entry as used.
  [[nodiscard]] auto mark_used(const std::string_view interrupt_id) -> result<void> {
    const auto iter = entries_.find(interrupt_id);
    if (iter == entries_.end()) {
      return result<void>::failure(errc::not_found);
    }
    if (iter->second.used) {
      return result<void>::failure(errc::contract_violation);
    }

    remove_active_location(iter->second.location);
    iter->second.used = true;
    return {};
  }

  /// Returns whether one entry has been consumed/used.
  [[nodiscard]] auto is_used(const std::string_view interrupt_id) const noexcept -> bool {
    const auto iter = entries_.find(interrupt_id);
    if (iter == entries_.end()) {
      return false;
    }
    return iter->second.used;
  }

private:
  using location_count_map =
      std::unordered_map<std::string, std::size_t, detail::transparent_string_hash,
                         detail::transparent_string_equal>;
  using entry_map = std::unordered_map<std::string, resume_entry, detail::transparent_string_hash,
                                       detail::transparent_string_equal>;

  /// Decrements map counter for one location key.
  auto decrement_location_count(location_count_map &counts, const std::string_view key) -> void {
    const auto iter = counts.find(key);
    if (iter == counts.end()) {
      return;
    }
    if (iter->second <= 1U) {
      counts.erase(iter);
      return;
    }
    --iter->second;
  }

  /// Adds location into active prefix/exact counters.
  auto add_active_location(const address &location) -> void {
    ++active_entry_count_;

    std::string key;
    const auto segments = location.segments();
    for (std::size_t index = 0U; index < segments.size(); ++index) {
      if (index != 0U) {
        key.push_back('/');
      }
      key += segments[index];
      ++active_prefix_counts_[key];
    }
    ++active_exact_counts_[key];
  }

  /// Removes location from active prefix/exact counters.
  auto remove_active_location(const address &location) -> void {
    if (active_entry_count_ > 0U) {
      --active_entry_count_;
    }

    std::string key;
    const auto segments = location.segments();
    for (std::size_t index = 0U; index < segments.size(); ++index) {
      if (index != 0U) {
        key.push_back('/');
      }
      key += segments[index];
      decrement_location_count(active_prefix_counts_, key);
    }
    decrement_location_count(active_exact_counts_, key);
  }

  /// Materializes normalized path key used by prefix index.
  [[nodiscard]] static auto materialize_location_key(const address &location) -> std::string {
    const auto segments = location.segments();
    std::size_t total_size = 2U;
    for (const auto &segment : segments) {
      total_size += segment.size() + 1U;
    }

    std::string key{};
    key.reserve(total_size);
    key.push_back('/');
    for (const auto &segment : segments) {
      key += segment;
      key.push_back('/');
    }
    return key;
  }

  /// Indexes location key for subtree lookup.
  auto index_location(const std::string_view interrupt_id, const std::string_view materialized_path)
      -> void {
    path_index_.insert(materialized_path, interrupt_id);
  }

  /// Removes location key from subtree index.
  auto unindex_location(const std::string_view interrupt_id) -> void {
    path_index_.erase(interrupt_id);
  }

  entry_map entries_{};
  detail::path_prefix_index path_index_{};
  location_count_map active_prefix_counts_{};
  location_count_map active_exact_counts_{};
  std::size_t active_entry_count_{0U};
};

template <> struct any_owned_traits<interrupt_snapshot> {
  [[nodiscard]] static auto into_owned(const interrupt_snapshot &value)
      -> result<interrupt_snapshot> {
    interrupt_snapshot owned{};
    owned.interrupt_id_to_address = value.interrupt_id_to_address;
    owned.interrupt_id_to_state.reserve(value.interrupt_id_to_state.size());
    for (const auto &[interrupt_id, state] : value.interrupt_id_to_state) {
      auto owned_state = wh::core::into_owned(state);
      if (owned_state.has_error()) {
        return result<interrupt_snapshot>::failure(owned_state.error());
      }
      owned.interrupt_id_to_state.insert_or_assign(interrupt_id, std::move(owned_state).value());
    }
    return owned;
  }

  [[nodiscard]] static auto into_owned(interrupt_snapshot &&value) -> result<interrupt_snapshot> {
    interrupt_snapshot owned{};
    owned.interrupt_id_to_address = std::move(value.interrupt_id_to_address);
    owned.interrupt_id_to_state.reserve(value.interrupt_id_to_state.size());
    for (auto iter = value.interrupt_id_to_state.begin();
         iter != value.interrupt_id_to_state.end();) {
      auto node = value.interrupt_id_to_state.extract(iter++);
      auto owned_state = wh::core::into_owned(std::move(node.mapped()));
      if (owned_state.has_error()) {
        return result<interrupt_snapshot>::failure(owned_state.error());
      }
      node.mapped() = std::move(owned_state).value();
      owned.interrupt_id_to_state.insert(std::move(node));
    }
    value = interrupt_snapshot{};
    return owned;
  }
};

template <> struct any_owned_traits<resume_state> {
  [[nodiscard]] static auto into_owned(const resume_state &value) -> result<resume_state> {
    resume_state owned{};
    owned.entries_.reserve(value.entries_.size());
    for (const auto &[interrupt_id, entry] : value.entries_) {
      auto owned_data = wh::core::into_owned(entry.data);
      if (owned_data.has_error()) {
        return result<resume_state>::failure(owned_data.error());
      }

      auto materialized_path = entry.materialized_path.empty()
                                   ? resume_state::materialize_location_key(entry.location)
                                   : entry.materialized_path;
      auto inserted = owned.entries_
                          .insert_or_assign(interrupt_id,
                                            resume_state::resume_entry{
                                                entry.location, std::move(owned_data).value(),
                                                std::move(materialized_path), entry.used})
                          .first;
      owned.index_location(inserted->first, inserted->second.materialized_path);
      if (!inserted->second.used) {
        owned.add_active_location(inserted->second.location);
      }
    }
    return owned;
  }

  [[nodiscard]] static auto into_owned(resume_state &&value) -> result<resume_state> {
    resume_state owned{};
    owned.entries_.reserve(value.entries_.size());
    for (auto iter = value.entries_.begin(); iter != value.entries_.end();) {
      auto node = value.entries_.extract(iter++);
      auto &entry = node.mapped();
      auto owned_data = wh::core::into_owned(std::move(entry.data));
      if (owned_data.has_error()) {
        return result<resume_state>::failure(owned_data.error());
      }

      auto materialized_path = entry.materialized_path.empty()
                                   ? resume_state::materialize_location_key(entry.location)
                                   : std::move(entry.materialized_path);
      entry.data = std::move(owned_data).value();
      entry.materialized_path = std::move(materialized_path);

      auto inserted = owned.entries_.insert(std::move(node));
      owned.index_location(inserted.position->first, inserted.position->second.materialized_path);
      if (!inserted.position->second.used) {
        owned.add_active_location(inserted.position->second.location);
      }
    }
    value = resume_state{};
    return owned;
  }
};

} // namespace wh::core
