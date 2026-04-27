// Defines the minimal authored chat shell that binds one model and optional
// instruction text without introducing a second chat runtime.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "wh/agent/agent.hpp"
#include "wh/agent/instruction.hpp"
#include "wh/agent/model_binding.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Stable internal key used for the authored chat-model node.
inline constexpr std::string_view chat_model_node_key = "__chat_model__";

/// Final-output storage mode used by simple chat agents.
enum class chat_output_mode {
  /// Store the final assistant message under the configured output key.
  value = 0U,
  /// Store the rendered final assistant text under the configured output key.
  text,
};

/// Thin authored chat shell that captures prompt and model binding without
/// embedding execution or transport logic.
class chat {
public:
  /// Creates one authored chat shell from the required name and description.
  chat(std::string name, std::string description) noexcept
      : name_(std::move(name)), description_(std::move(description)) {}

  chat(const chat &) = delete;
  auto operator=(const chat &) -> chat & = delete;
  chat(chat &&) noexcept = default;
  auto operator=(chat &&) noexcept -> chat & = default;
  ~chat() = default;

  /// Returns the authored shell name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns the authored shell description.
  [[nodiscard]] auto description() const noexcept -> std::string_view { return description_; }

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
  [[nodiscard]] auto render_instruction(const std::string_view separator = "\n") const
      -> std::string {
    return instruction_.render(separator);
  }

  /// Sets the optional output slot name written at graph exit.
  auto set_output_key(std::string output_key) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    output_key_ = std::move(output_key);
    return {};
  }

  /// Selects how the configured output slot is materialized.
  auto set_output_mode(const chat_output_mode mode) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    output_mode_ = mode;
    return {};
  }

  /// Installs the semantic model binding used by this shell.
  auto set_model(wh::agent::model_binding binding) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    model_binding_ = std::move(binding);
    return {};
  }

  /// Returns the semantic model binding used by this shell.
  [[nodiscard]] auto model_binding() const
      -> wh::core::result<std::reference_wrapper<const wh::agent::model_binding>> {
    if (!model_binding_.has_value()) {
      return wh::core::result<std::reference_wrapper<const wh::agent::model_binding>>::failure(
          wh::core::errc::not_found);
    }
    return std::cref(*model_binding_);
  }

  /// Returns the configured output slot name.
  [[nodiscard]] auto output_key() const noexcept -> std::string_view { return output_key_; }

  /// Returns the selected output materialization mode.
  [[nodiscard]] auto output_mode() const noexcept -> chat_output_mode { return output_mode_; }

  /// Converts this frozen authored shell into the common executable agent surface.
  [[nodiscard]] auto into_agent() && -> wh::core::result<wh::agent::agent>;

  /// Validates authored configuration and freezes this shell once.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || description_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!model_binding_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    frozen_ = true;
    return {};
  }

private:
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    return {};
  }

  std::string name_{};
  std::string description_{};
  wh::agent::instruction instruction_{};
  std::optional<wh::agent::model_binding> model_binding_{};
  std::string output_key_{};
  chat_output_mode output_mode_{chat_output_mode::value};
  bool frozen_{false};
};

} // namespace wh::agent
