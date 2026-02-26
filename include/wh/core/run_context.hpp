#pragma once

#include <any>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/resume_state.hpp"

namespace wh::internal {
class callback_manager;
}

namespace wh::core {

namespace detail {

struct session_string_hash {
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

struct session_string_equal {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view left,
                                const std::string_view right) const noexcept
      -> bool {
    return left == right;
  }
};

} // namespace detail

struct run_context {
  using session_store =
      std::unordered_map<std::string, std::any, detail::session_string_hash,
                         detail::session_string_equal>;

  session_store session_values{};
  std::shared_ptr<wh::internal::callback_manager> callback_manager{};
  std::optional<interrupt_context> interrupt_info{};
  std::optional<resume_state> resume_info{};
};

template <typename value_t>
auto set_session_value(run_context &context, std::string key, value_t &&value)
    -> void {
  using stored_t = std::remove_cvref_t<value_t>;
  if constexpr (std::same_as<stored_t, std::any>) {
    context.session_values.insert_or_assign(std::move(key),
                                            std::forward<value_t>(value));
  } else {
    context.session_values.insert_or_assign(
        std::move(key),
        std::any{std::in_place_type<stored_t>, std::forward<value_t>(value)});
  }
}

template <typename value_t>
[[nodiscard]] auto session_value_ref(const run_context &context,
                                     const std::string_view key)
    -> result<std::reference_wrapper<const value_t>> {
  const auto iter = context.session_values.find(key);
  if (iter == context.session_values.end()) {
    return result<std::reference_wrapper<const value_t>>::failure(
        errc::not_found);
  }

  const auto *typed = std::any_cast<value_t>(&iter->second);
  if (typed == nullptr) {
    return result<std::reference_wrapper<const value_t>>::failure(
        errc::type_mismatch);
  }
  return std::cref(*typed);
}

template <typename value_t>
[[nodiscard]] auto session_value_ref(run_context &context,
                                     const std::string_view key)
    -> result<std::reference_wrapper<value_t>> {
  const auto iter = context.session_values.find(key);
  if (iter == context.session_values.end()) {
    return result<std::reference_wrapper<value_t>>::failure(errc::not_found);
  }

  auto *typed = std::any_cast<value_t>(&iter->second);
  if (typed == nullptr) {
    return result<std::reference_wrapper<value_t>>::failure(
        errc::type_mismatch);
  }
  return std::ref(*typed);
}

template <typename value_t>
[[nodiscard]] auto consume_session_value(run_context &context,
                                         const std::string_view key)
    -> result<value_t> {
  const auto iter = context.session_values.find(key);
  if (iter == context.session_values.end()) {
    return result<value_t>::failure(errc::not_found);
  }

  auto *typed = std::any_cast<value_t>(&iter->second);
  if (typed == nullptr) {
    return result<value_t>::failure(errc::type_mismatch);
  }

  value_t moved = std::move(*typed);
  context.session_values.erase(iter);
  return moved;
}

[[nodiscard]] inline auto
has_callback_manager(const run_context &context) noexcept -> bool {
  return static_cast<bool>(context.callback_manager);
}

} // namespace wh::core
