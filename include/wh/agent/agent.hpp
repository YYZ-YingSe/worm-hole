// Defines the authored agent entity used by business-layer composition without
// creating a parallel runtime or mutable session store.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/agent/instruction.hpp"
#include "wh/compose/graph.hpp"
#include "wh/core/any.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/schema/message.hpp"

namespace wh::agent {

/// One normalized transfer action emitted by an agent-family shell.
struct agent_transfer {
  /// Stable destination agent name requested by the current agent.
  std::string target_agent_name{};
  /// Stable tool-call id associated with the transfer handoff.
  std::string tool_call_id{};
};

/// Common graph-boundary result emitted by all executable agent shells.
struct agent_output {
  /// Final message produced by the lowered agent graph.
  wh::schema::message final_message{};
  /// Externally visible conversation history emitted by the lowered agent run.
  std::vector<wh::schema::message> history_messages{};
  /// Optional normalized transfer action emitted by the lowered agent run.
  std::optional<agent_transfer> transfer{};
  /// Explicit output slots materialized by the lowered agent graph.
  std::unordered_map<std::string, wh::core::any, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      output_values{};
};

template <typename value_t>
/// Typed extractor that reads one structured value from `agent_output`.
using output_reader = wh::core::callback_function<wh::core::result<value_t>(
    const agent_output &, wh::core::run_context &) const>;

/// Mutable authored agent entity with child topology, instruction fragments,
/// and transfer whitelist rules that freeze before execution.
class agent {
public:
  /// Freeze hook used by executable authored agents before graph lowering.
  using freeze_hook = wh::core::move_only_function<wh::core::result<void>()>;

  /// Lower hook that materializes one frozen executable agent into one graph.
  using lower_hook = wh::core::move_only_function<wh::core::result<wh::compose::graph>()>;

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
  [[nodiscard]] auto parent_name() const noexcept -> std::optional<std::string_view> {
    if (!parent_name_.has_value()) {
      return std::nullopt;
    }
    return std::string_view{*parent_name_};
  }

  /// Returns true after authoring has been frozen successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Returns the optional human-readable agent description.
  [[nodiscard]] auto description() const noexcept -> std::string_view { return description_; }

  /// Returns true when this authored agent can lower into one compose graph.
  [[nodiscard]] auto executable() const noexcept -> bool { return static_cast<bool>(lower_); }

  /// Replaces the current agent description before freeze.
  auto set_description(std::string description) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    description_ = std::move(description);
    return {};
  }

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
  [[nodiscard]] auto render_instruction(const std::string_view separator = "\n") const
      -> std::string {
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
  auto allow_transfer_to_child(std::string child_name) -> wh::core::result<void> {
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
  [[nodiscard]] auto has_child(const std::string_view child_name) const noexcept -> bool {
    for (const auto &child : children_) {
      if (child->name_ == child_name) {
        return true;
      }
    }
    return false;
  }

  /// Returns true when transfer to the named child is whitelisted.
  [[nodiscard]] auto allows_transfer_to_child(const std::string_view child_name) const noexcept
      -> bool {
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
  [[nodiscard]] auto child_count() const noexcept -> std::size_t { return children_.size(); }

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
  [[nodiscard]] auto allowed_transfer_children() const -> std::vector<std::string> {
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
    if (freeze_) {
      auto frozen = freeze_();
      if (frozen.has_error()) {
        return frozen;
      }
    }
    frozen_ = true;
    return {};
  }

  /// Installs executable lowering hooks before freeze.
  auto bind_execution(freeze_hook freeze, lower_hook lower) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!lower) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    freeze_ = std::move(freeze);
    lower_ = std::move(lower);
    return {};
  }

  /// Lowers this frozen executable agent into its native compose graph surface.
  [[nodiscard]] auto lower() const -> wh::core::result<wh::compose::graph> {
    if (!frozen_) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::contract_violation);
    }
    if (!lower_) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::not_supported);
    }
    return lower_();
  }

private:
  /// Rejects topology mutation after freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Stable authored agent name.
  std::string name_{};
  /// Optional human-readable description used by agent-family wrappers.
  std::string description_{};
  /// Stable parent name once this agent is adopted.
  std::optional<std::string> parent_name_{};
  /// Mutable instruction fragments rendered at lowering time.
  wh::agent::instruction instruction_{};
  /// Owned child agents attached to this authored entity.
  std::vector<std::unique_ptr<agent>> children_{};
  /// Whitelisted downward transfer targets keyed by child name.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      allowed_transfer_children_{};
  /// True when transfer back to the parent is allowed.
  bool allow_transfer_to_parent_{false};
  /// Optional freeze hook owned by executable authored agents.
  freeze_hook freeze_{nullptr};
  /// Optional lowering hook owned by executable authored agents.
  mutable lower_hook lower_{nullptr};
  /// True after topology and transfer rules are frozen.
  bool frozen_{false};
};

} // namespace wh::agent
