#pragma once

#include <algorithm>
#include <any>
#include <cstdint>
#include <functional>
#include <iterator>
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
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::core {

namespace detail {

struct transparent_string_hash {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view value) const noexcept
      -> std::size_t {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] auto operator()(const std::string &value) const noexcept
      -> std::size_t {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] auto operator()(const char *value) const noexcept
      -> std::size_t {
    return std::hash<std::string_view>{}(value);
  }
};

struct transparent_string_equal {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view left,
                                const std::string_view right) const noexcept
      -> bool {
    return left == right;
  }
};

} // namespace detail

struct interrupt_signal {
  std::string interrupt_id{};
  address location{};
  std::any state{};
  std::any layer_payload{};
  bool used{false};
};

struct interrupt_context {
  std::string interrupt_id{};
  address location{};
  std::any state{};
  std::any layer_payload{};
  bool used{false};
};

struct interrupt_signal_tree_node {
  address location{};
  std::vector<interrupt_signal> signals{};
  std::vector<interrupt_signal_tree_node> children{};
};

struct interrupt_context_tree_node {
  address location{};
  std::vector<interrupt_context> contexts{};
  std::vector<interrupt_context_tree_node> children{};
};

[[nodiscard]] inline auto to_interrupt_context(const interrupt_signal &signal)
    -> interrupt_context {
  return interrupt_context{signal.interrupt_id, signal.location, signal.state,
                           signal.layer_payload, signal.used};
}

[[nodiscard]] inline auto to_interrupt_context(interrupt_signal &&signal)
    -> interrupt_context {
  return interrupt_context{std::move(signal.interrupt_id),
                           std::move(signal.location), std::move(signal.state),
                           std::move(signal.layer_payload), signal.used};
}

[[nodiscard]] inline auto to_interrupt_signal(const interrupt_context &context)
    -> interrupt_signal {
  return interrupt_signal{context.interrupt_id, context.location, context.state,
                          context.layer_payload, context.used};
}

[[nodiscard]] inline auto to_interrupt_signal(interrupt_context &&context)
    -> interrupt_signal {
  return interrupt_signal{std::move(context.interrupt_id),
                          std::move(context.location), std::move(context.state),
                          std::move(context.layer_payload), context.used};
}

namespace detail {

template <typename node_t>
auto find_child_by_location(std::vector<node_t> &children,
                            const address &location) -> node_t * {
  const auto iter =
      std::ranges::find_if(children, [&](const node_t &node) -> bool {
        return node.location == location;
      });
  if (iter == children.end()) {
    return nullptr;
  }
  return std::addressof(*iter);
}

template <typename node_t>
auto ensure_tree_path(std::vector<node_t> &roots, const address &location)
    -> node_t * {
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

inline auto append_filtered_segment(
    address &target, const std::string_view segment,
    const std::span<const std::string_view> allowed_segments) -> void {
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

[[nodiscard]] inline auto
project_address(const address &source,
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

[[nodiscard]] inline auto project_interrupt_context(
    const interrupt_context &context,
    const std::span<const std::string_view> allowed_segments)
    -> interrupt_context {
  if (allowed_segments.empty()) {
    return context;
  }
  interrupt_context projected{context};
  projected.location = project_address(context.location, allowed_segments);
  return projected;
}

[[nodiscard]] inline auto project_interrupt_context(
    interrupt_context &&context,
    const std::span<const std::string_view> allowed_segments)
    -> interrupt_context {
  if (allowed_segments.empty()) {
    return std::move(context);
  }
  context.location = project_address(context.location, allowed_segments);
  return std::move(context);
}

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

[[nodiscard]] inline auto rebuild_interrupt_context_tree(
    const std::span<const interrupt_context> contexts)
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

inline auto flatten_signal_tree_node(const interrupt_signal_tree_node &node,
                                     std::vector<interrupt_signal> &output)
    -> void {
  output.insert(output.end(), node.signals.begin(), node.signals.end());
  for (const auto &child : node.children) {
    flatten_signal_tree_node(child, output);
  }
}

inline auto flatten_context_tree_node(const interrupt_context_tree_node &node,
                                      std::vector<interrupt_context> &output)
    -> void {
  output.insert(output.end(), node.contexts.begin(), node.contexts.end());
  for (const auto &child : node.children) {
    flatten_context_tree_node(child, output);
  }
}

} // namespace detail

[[nodiscard]] inline auto to_interrupt_context_tree(
    const std::span<const interrupt_signal_tree_node> roots)
    -> std::vector<interrupt_context_tree_node> {
  std::vector<interrupt_context_tree_node> converted{};
  converted.reserve(roots.size());
  for (const auto &root : roots) {
    converted.push_back(detail::to_context_tree_node(root));
  }
  return converted;
}

[[nodiscard]] inline auto to_interrupt_signal_tree(
    const std::span<const interrupt_context_tree_node> roots)
    -> std::vector<interrupt_signal_tree_node> {
  std::vector<interrupt_signal_tree_node> converted{};
  converted.reserve(roots.size());
  for (const auto &root : roots) {
    converted.push_back(detail::to_signal_tree_node(root));
  }
  return converted;
}

[[nodiscard]] inline auto flatten_interrupt_signal_tree(
    const std::span<const interrupt_signal_tree_node> roots)
    -> std::vector<interrupt_signal> {
  std::vector<interrupt_signal> flattened{};
  for (const auto &root : roots) {
    detail::flatten_signal_tree_node(root, flattened);
  }
  return flattened;
}

[[nodiscard]] inline auto flatten_interrupt_context_tree(
    const std::span<const interrupt_context_tree_node> roots)
    -> std::vector<interrupt_context> {
  std::vector<interrupt_context> flattened{};
  for (const auto &root : roots) {
    detail::flatten_context_tree_node(root, flattened);
  }
  return flattened;
}

struct interrupt_snapshot {
  using address_map =
      std::unordered_map<std::string, address, detail::transparent_string_hash,
                         detail::transparent_string_equal>;
  using state_map =
      std::unordered_map<std::string, std::any, detail::transparent_string_hash,
                         detail::transparent_string_equal>;

  address_map interrupt_id_to_address{};
  state_map interrupt_id_to_state{};
};

[[nodiscard]] inline auto
flatten_interrupt_signals(const std::span<const interrupt_signal> signals)
    -> interrupt_snapshot {
  interrupt_snapshot snapshot{};
  snapshot.interrupt_id_to_address.reserve(signals.size());
  snapshot.interrupt_id_to_state.reserve(signals.size());

  for (const auto &signal : signals) {
    snapshot.interrupt_id_to_address.insert_or_assign(signal.interrupt_id,
                                                      signal.location);
    snapshot.interrupt_id_to_state.insert_or_assign(signal.interrupt_id,
                                                    signal.state);
  }
  return snapshot;
}

[[nodiscard]] inline auto
flatten_interrupt_signals(std::vector<interrupt_signal> &&signals)
    -> interrupt_snapshot {
  interrupt_snapshot snapshot{};
  snapshot.interrupt_id_to_address.reserve(signals.size());
  snapshot.interrupt_id_to_state.reserve(signals.size());

  for (auto &signal : signals) {
    snapshot.interrupt_id_to_address.insert_or_assign(
        signal.interrupt_id, std::move(signal.location));
    snapshot.interrupt_id_to_state.insert_or_assign(signal.interrupt_id,
                                                    std::move(signal.state));
  }
  return snapshot;
}

class resume_state {
public:
  struct resume_entry {
    address location{};
    std::any data{};
    bool used{false};
  };

  template <typename value_t>
  auto upsert(std::string interrupt_id, address location, value_t &&data)
      -> result<void> {
    using stored_t = std::remove_cvref_t<value_t>;
    if constexpr (std::same_as<stored_t, std::any>) {
      return upsert(std::move(interrupt_id), std::move(location),
                    std::forward<value_t>(data));
    } else {
      return upsert(
          std::move(interrupt_id), std::move(location),
          std::any{std::in_place_type<stored_t>, std::forward<value_t>(data)});
    }
  }

  auto upsert(std::string interrupt_id, address location, std::any data)
      -> result<void> {
    if (interrupt_id.empty()) {
      return result<void>::failure(errc::invalid_argument);
    }

    const auto iter = entries_.find(interrupt_id);
    if (iter != entries_.end() && !iter->second.used) {
      remove_active_location(iter->second.location);
    }

    auto [updated, inserted] = entries_.insert_or_assign(
        std::move(interrupt_id),
        resume_entry{std::move(location), std::move(data), false});
    static_cast<void>(inserted);
    add_active_location(updated->second.location);
    return {};
  }

  auto merge(const resume_state &other) -> result<void> {
    if (this == &other) {
      return {};
    }
    if (revision_ != 0U && other.revision_ != 0U &&
        revision_ != other.revision_) {
      return result<void>::failure(errc::contract_violation);
    }
    if (revision_ == 0U) {
      revision_ = other.revision_;
    }
    entries_.reserve(entries_.size() + other.entries_.size());
    for (const auto &[interrupt_id, entry] : other.entries_) {
      const auto current = entries_.find(interrupt_id);
      if (current != entries_.end() && !current->second.used) {
        remove_active_location(current->second.location);
      }
      entries_.insert_or_assign(interrupt_id, entry);
      if (!entry.used) {
        add_active_location(entry.location);
      }
    }
    return {};
  }

  auto merge(resume_state &&other) -> result<void> {
    if (this == &other) {
      return {};
    }
    if (revision_ != 0U && other.revision_ != 0U &&
        revision_ != other.revision_) {
      return result<void>::failure(errc::contract_violation);
    }
    if (revision_ == 0U) {
      revision_ = other.revision_;
    }

    entries_.reserve(entries_.size() + other.entries_.size());
    for (auto &[interrupt_id, entry] : other.entries_) {
      const auto current = entries_.find(interrupt_id);
      if (current != entries_.end() && !current->second.used) {
        remove_active_location(current->second.location);
      }

      auto [updated, inserted] =
          entries_.insert_or_assign(interrupt_id, std::move(entry));
      static_cast<void>(inserted);
      if (!updated->second.used) {
        add_active_location(updated->second.location);
      }
    }

    other.entries_.clear();
    other.active_prefix_counts_.clear();
    other.active_exact_counts_.clear();
    other.active_entry_count_ = 0U;
    other.revision_ = 0U;
    return {};
  }

  auto bind_revision(const std::uint64_t revision) noexcept -> void {
    revision_ = revision;
  }

  [[nodiscard]] auto revision() const noexcept -> std::uint64_t {
    return revision_;
  }

  [[nodiscard]] auto
  contains_interrupt_id(const std::string_view interrupt_id) const noexcept
      -> bool {
    return entries_.contains(interrupt_id);
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return entries_.size();
  }

  [[nodiscard]] auto is_resume_target(const address &location) const noexcept
      -> bool {
    if (active_entry_count_ == 0U) {
      return false;
    }
    if (location.empty()) {
      return true;
    }

    return active_prefix_counts_.contains(location.to_string());
  }

  [[nodiscard]] auto
  is_exact_resume_target(const address &location) const noexcept -> bool {
    const auto iter = active_exact_counts_.find(location.to_string());
    return iter != active_exact_counts_.end() && iter->second > 0U;
  }

  [[nodiscard]] auto next_resume_points(const address &location) const
      -> std::vector<std::string> {
    const auto parent_depth = location.size();
    std::vector<std::string> child_points{};
    child_points.reserve(entries_.size());
    auto child_point_view =
        entries_ | std::views::values |
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

  template <typename value_t>
  [[nodiscard]] auto consume(const std::string_view interrupt_id)
      -> result<value_t> {
    const auto iter = entries_.find(interrupt_id);
    if (iter == entries_.end()) {
      return result<value_t>::failure(errc::not_found);
    }
    if (iter->second.used) {
      return result<value_t>::failure(errc::contract_violation);
    }

    auto *typed = std::any_cast<value_t>(&iter->second.data);
    if (typed == nullptr) {
      return result<value_t>::failure(errc::type_mismatch);
    }

    value_t moved = std::move(*typed);
    remove_active_location(iter->second.location);
    iter->second.used = true;
    return moved;
  }

  [[nodiscard]] auto mark_used(const std::string_view interrupt_id)
      -> result<void> {
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

  [[nodiscard]] auto is_used(const std::string_view interrupt_id) const noexcept
      -> bool {
    const auto iter = entries_.find(interrupt_id);
    if (iter == entries_.end()) {
      return false;
    }
    return iter->second.used;
  }

private:
  using location_count_map =
      std::unordered_map<std::string, std::size_t,
                         detail::transparent_string_hash,
                         detail::transparent_string_equal>;
  using entry_map = std::unordered_map<std::string, resume_entry,
                                       detail::transparent_string_hash,
                                       detail::transparent_string_equal>;

  auto decrement_location_count(location_count_map &counts,
                                const std::string_view key) -> void {
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

  entry_map entries_{};
  location_count_map active_prefix_counts_{};
  location_count_map active_exact_counts_{};
  std::size_t active_entry_count_{0U};
  std::uint64_t revision_{0U};
};

} // namespace wh::core
