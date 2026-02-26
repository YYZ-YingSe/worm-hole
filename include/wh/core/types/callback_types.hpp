#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>

#include "wh/core/error.hpp"

namespace wh::core {

enum class callback_stage : std::uint8_t {
  start = 0U,
  end,
  error,
  stream_start,
  stream_end,
};

[[nodiscard]] constexpr auto
is_reverse_callback_stage(const callback_stage stage) noexcept -> bool {
  return stage == callback_stage::start;
}

struct callback_event_view {
  const void *payload{nullptr};
  std::type_index payload_type{typeid(void)};

  template <typename payload_t>
  [[nodiscard]] static auto from(const payload_t &value) noexcept
      -> callback_event_view {
    return callback_event_view{std::addressof(value),
                               std::type_index(typeid(payload_t))};
  }

  template <typename payload_t>
  [[nodiscard]] auto get_if() const noexcept -> const payload_t * {
    if (payload_type != std::type_index(typeid(payload_t))) {
      return nullptr;
    }
    return static_cast<const payload_t *>(payload);
  }
};

template <typename payload_t>
[[nodiscard]] inline auto
make_callback_event_view(const payload_t &value) noexcept
    -> callback_event_view {
  return callback_event_view::from(value);
}

using callback_timing_checker = std::function<bool(callback_stage)>;
using callback_stage_handler =
    std::function<void(callback_stage, callback_event_view)>;

struct callback_handler_config {
  callback_timing_checker timing_checker{};
  std::string name{};
};

struct callback_fatal_error {
  error_code code{errc::internal_error};
  std::string exception_message{};
  std::string call_stack{};

  [[nodiscard]] auto to_string() const -> std::string {
    return std::string{"code="} + code.message() +
           ", exception=" + exception_message + ", stack=" + call_stack;
  }
};

} // namespace wh::core
