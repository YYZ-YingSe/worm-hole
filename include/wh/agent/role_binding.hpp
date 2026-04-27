// Defines one semantic authored role-binding wrapper used by business-layer
// shells to preserve native role capability until lower time.
#pragma once

#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "wh/agent/agent.hpp"
#include "wh/compose/graph.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Semantic authored binding for one role slot.
class role_binding {
public:
  using freeze_fn = wh::core::move_only_function<wh::core::result<void>()>;
  using lower_fn = wh::core::move_only_function<wh::core::result<wh::compose::graph>() const>;

  role_binding() noexcept = default;

  role_binding(std::string name, std::string description, freeze_fn freeze, lower_fn lower,
               const bool frozen = false, const bool executable = true) noexcept
      : name_(std::move(name)), description_(std::move(description)), freeze_(std::move(freeze)),
        lower_(std::move(lower)), frozen_(frozen), executable_(executable) {}

  [[nodiscard]] explicit operator bool() const noexcept { return !name_.empty() && executable_; }

  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  [[nodiscard]] auto description() const noexcept -> std::string_view { return description_; }

  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  [[nodiscard]] auto executable() const noexcept -> bool { return executable_; }

  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || !lower_) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!executable_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (freeze_) {
      auto status = freeze_();
      if (status.has_error()) {
        return status;
      }
    }
    frozen_ = true;
    return {};
  }

  [[nodiscard]] auto lower() const -> wh::core::result<wh::compose::graph> {
    if (!frozen_) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::contract_violation);
    }
    if (!lower_ || !executable_) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::not_supported);
    }
    return lower_();
  }

private:
  std::string name_{};
  std::string description_{};
  freeze_fn freeze_{nullptr};
  lower_fn lower_{nullptr};
  bool frozen_{false};
  bool executable_{false};
};

namespace detail {

template <typename role_t>
concept lowerable_role_provider = requires(role_t &value) {
  { value.name() } -> std::convertible_to<std::string_view>;
  { value.freeze() } -> std::same_as<wh::core::result<void>>;
  { value.lower() } -> std::same_as<wh::core::result<wh::compose::graph>>;
};

template <typename role_t>
concept into_agent_role_provider = requires(role_t &value) {
  { value.name() } -> std::convertible_to<std::string_view>;
  { value.freeze() } -> std::same_as<wh::core::result<void>>;
  { std::move(value).into_agent() } -> std::same_as<wh::core::result<wh::agent::agent>>;
};

template <typename role_t> struct role_binding_box {
  template <typename role_u>
  explicit role_binding_box(role_u &&value) : role(std::forward<role_u>(value)) {}

  std::optional<role_t> role{};
  std::optional<wh::agent::agent> exported{};
};

template <typename role_t>
[[nodiscard]] inline auto role_description(const role_t &value) -> std::string {
  if constexpr (requires { value.description(); }) {
    return std::string{value.description()};
  } else {
    return std::string{};
  }
}

template <typename role_t> [[nodiscard]] inline auto role_frozen(const role_t &value) -> bool {
  if constexpr (requires { value.frozen(); }) {
    return value.frozen();
  } else {
    return false;
  }
}

template <typename role_t> [[nodiscard]] inline auto role_executable(const role_t &value) -> bool {
  if constexpr (requires { value.executable(); }) {
    return value.executable();
  } else {
    return true;
  }
}

} // namespace detail

/// Preserves one already-lowerable executable role surface.
[[nodiscard]] inline auto make_role_binding(wh::agent::agent value) -> wh::agent::role_binding {
  auto storage = std::make_shared<wh::agent::agent>(std::move(value));
  return wh::agent::role_binding{
      std::string{storage->name()},
      std::string{storage->description()},
      [storage]() mutable -> wh::core::result<void> { return storage->freeze(); },
      [storage]() -> wh::core::result<wh::compose::graph> { return storage->lower(); },
      storage->frozen(),
      storage->executable(),
  };
}

/// Preserves one native role provider that already exposes `freeze()` and
/// `lower()` directly.
template <typename role_t>
  requires detail::lowerable_role_provider<std::remove_cvref_t<role_t>> &&
           (!std::same_as<std::remove_cvref_t<role_t>, wh::agent::agent>) &&
           (!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
[[nodiscard]] inline auto make_role_binding(role_t &&value) -> wh::agent::role_binding {
  using stored_role_t = std::remove_cvref_t<role_t>;
  auto storage =
      std::make_shared<detail::role_binding_box<stored_role_t>>(std::forward<role_t>(value));
  return wh::agent::role_binding{
      std::string{storage->role->name()},
      detail::role_description(*storage->role),
      [storage]() mutable -> wh::core::result<void> { return storage->role->freeze(); },
      [storage]() -> wh::core::result<wh::compose::graph> { return storage->role->lower(); },
      detail::role_frozen(*storage->role),
      detail::role_executable(*storage->role),
  };
}

/// Preserves one authored shell that must first lower into the common
/// executable agent surface before graph lowering becomes available.
template <typename role_t>
  requires detail::into_agent_role_provider<std::remove_cvref_t<role_t>> &&
           (!detail::lowerable_role_provider<std::remove_cvref_t<role_t>>) &&
           (!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
[[nodiscard]] inline auto make_role_binding(role_t &&value) -> wh::agent::role_binding {
  using stored_role_t = std::remove_cvref_t<role_t>;
  auto storage =
      std::make_shared<detail::role_binding_box<stored_role_t>>(std::forward<role_t>(value));
  return wh::agent::role_binding{
      std::string{storage->role->name()},
      detail::role_description(*storage->role),
      [storage]() mutable -> wh::core::result<void> { return storage->role->freeze(); },
      [storage]() -> wh::core::result<wh::compose::graph> {
        if (!storage->exported.has_value()) {
          auto exported = std::move(*storage->role).into_agent();
          if (exported.has_error()) {
            return wh::core::result<wh::compose::graph>::failure(exported.error());
          }
          storage->exported.emplace(std::move(exported).value());
          storage->role.reset();
        }
        return storage->exported->lower();
      },
      detail::role_frozen(*storage->role),
      true,
  };
}

} // namespace wh::agent
