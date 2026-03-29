// Defines the authored agent entity used by ADK composition without creating
// a parallel runtime or mutable session store.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/adk/instruction.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::agent {

/// Mutable authored agent entity with child topology, instruction fragments,
/// and transfer whitelist rules that freeze before execution.
class agent {
public:
  /// Stores one authored agent name.
  explicit agent(std::string name) noexcept : name_(std::move(name)) {}

  agent(const agent &) = delete;
  auto operator=(const agent &) -> agent & = delete;
  agent(agent &&) noexcept = default;
  auto operator=(agent &&) noexcept -> agent & = default;
  ~agent() = default;

  /// Returns the stable authored agent name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns the stable parent agent name when this agent is attached.
  [[nodiscard]] auto parent_name() const noexcept
      -> std::optional<std::string_view> {
    if (!parent_name_.has_value()) {
      return std::nullopt;
    }
    return std::string_view{*parent_name_};
  }

  /// Returns true after authoring has been frozen successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Appends one instruction fragment before freeze.
  auto append_instruction(std::string text, const std::int32_t priority = 0)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    instruction_.append(std::move(text), priority);
    return {};
  }

  /// Replaces the current base instruction before freeze.
  auto replace_instruction(std::string text, const std::int32_t priority = 0)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    instruction_.replace(std::move(text), priority);
    return {};
  }

  /// Renders the authored instruction string.
  [[nodiscard]] auto render_instruction(
      const std::string_view separator = "\n") const -> std::string {
    return instruction_.render(separator);
  }

  /// Adopts one child agent before freeze.
  auto add_child(agent &&child) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (child.name_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (child.parent_name_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }
    if (has_child(child.name_)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    child.parent_name_ = name_;
    children_.push_back(std::make_unique<agent>(std::move(child)));
    return {};
  }

  /// Whitelists one downward transfer target by child name before freeze.
  auto allow_transfer_to_child(std::string child_name)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (child_name.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    allowed_transfer_children_.insert(std::move(child_name));
    return {};
  }

  /// Whitelists upward transfer back to the parent before freeze.
  auto allow_transfer_to_parent() -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    allow_transfer_to_parent_ = true;
    return {};
  }

  /// Returns true when the named child exists.
  [[nodiscard]] auto has_child(const std::string_view child_name) const noexcept
      -> bool {
    for (const auto &child : children_) {
      if (child->name_ == child_name) {
        return true;
      }
    }
    return false;
  }

  /// Returns true when transfer to the named child is whitelisted.
  [[nodiscard]] auto allows_transfer_to_child(
      const std::string_view child_name) const noexcept -> bool {
    return allowed_transfer_children_.contains(child_name);
  }

  /// Returns true when upward transfer to the parent is whitelisted.
  [[nodiscard]] auto allows_transfer_to_parent() const noexcept -> bool {
    return allow_transfer_to_parent_;
  }

  /// Returns one child agent by name.
  [[nodiscard]] auto child(const std::string_view child_name) const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    for (const auto &entry : children_) {
      if (entry->name_ == child_name) {
        return std::cref(*entry);
      }
    }
    return wh::core::result<std::reference_wrapper<const agent>>::failure(
        wh::core::errc::not_found);
  }

  /// Returns the current child count.
  [[nodiscard]] auto child_count() const noexcept -> std::size_t {
    return children_.size();
  }

  /// Returns one copied child-name list for diagnostics and tests.
  [[nodiscard]] auto child_names() const -> std::vector<std::string> {
    std::vector<std::string> names{};
    names.reserve(children_.size());
    for (const auto &child : children_) {
      names.push_back(child->name_);
    }
    return names;
  }

  /// Returns one copied transfer-child whitelist for diagnostics and tests.
  [[nodiscard]] auto allowed_transfer_children() const
      -> std::vector<std::string> {
    std::vector<std::string> names{};
    names.reserve(allowed_transfer_children_.size());
    for (const auto &entry : allowed_transfer_children_) {
      names.push_back(entry);
    }
    return names;
  }

  /// Validates topology and freezes this agent and all descendants.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    for (const auto &target : allowed_transfer_children_) {
      if (!has_child(target)) {
        return wh::core::result<void>::failure(wh::core::errc::not_found);
      }
    }
    for (auto &child : children_) {
      auto frozen = child->freeze();
      if (frozen.has_error()) {
        return frozen;
      }
    }
    frozen_ = true;
    return {};
  }

private:
  /// Rejects topology mutation after freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Stable authored agent name.
  std::string name_{};
  /// Stable parent name once this agent is adopted.
  std::optional<std::string> parent_name_{};
  /// Mutable instruction fragments rendered at lowering time.
  wh::adk::instruction instruction_{};
  /// Owned child agents attached to this authored entity.
  std::vector<std::unique_ptr<agent>> children_{};
  /// Whitelisted downward transfer targets keyed by child name.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      allowed_transfer_children_{};
  /// True when transfer back to the parent is allowed.
  bool allow_transfer_to_parent_{false};
  /// True after topology and transfer rules are frozen.
  bool frozen_{false};
};

} // namespace wh::agent
