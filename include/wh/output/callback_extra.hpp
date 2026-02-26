#pragma once

#include <any>
#include <functional>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::output {

struct callback_extra_payload {
  std::any payload{};
};

template <typename value_t>
[[nodiscard]] inline auto make_callback_extra_payload(value_t &&value)
    -> callback_extra_payload {
  return callback_extra_payload{std::any{std::forward<value_t>(value)}};
}

template <typename value_t>
[[nodiscard]] inline auto
callback_extra_get_if(const callback_extra_payload &extra) -> const value_t * {
  return std::any_cast<value_t>(&extra.payload);
}

template <typename value_t>
[[nodiscard]] inline auto callback_extra_as(const callback_extra_payload &extra)
    -> wh::core::result<value_t> {
  const auto *typed = callback_extra_get_if<value_t>(extra);
  if (typed == nullptr) {
    return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
  }
  return *typed;
}

template <typename value_t>
[[nodiscard]] inline auto
callback_extra_cref_as(const callback_extra_payload &extra)
    -> wh::core::result<std::reference_wrapper<const value_t>> {
  const auto *typed = callback_extra_get_if<value_t>(extra);
  if (typed == nullptr) {
    return wh::core::result<std::reference_wrapper<const value_t>>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::cref(*typed);
}

struct callback_extra_view {
  const void *payload{nullptr};
  std::type_index payload_type{typeid(void)};

  template <typename value_t>
  [[nodiscard]] static auto from(const value_t &value) -> callback_extra_view {
    return callback_extra_view{std::addressof(value),
                               std::type_index(typeid(value_t))};
  }

  template <typename value_t>
  [[nodiscard]] auto get_if() const -> const value_t * {
    if (payload_type != std::type_index(typeid(value_t))) {
      return nullptr;
    }
    return static_cast<const value_t *>(payload);
  }
};

} // namespace wh::output
