// Defines the canonical stream control markers and stream read result aliases.
#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>
#include <variant>

#include "wh/core/result.hpp"

namespace wh::schema::stream {

/// Non-error control markers surfaced by non-blocking stream reads.
enum class stream_signal : std::uint8_t {
  /// `try_read()` has no chunk available right now.
  pending = 1U,
};

/// Stable string form for stream control markers.
[[nodiscard]] constexpr auto to_string(const stream_signal signal) noexcept
    -> std::string_view {
  switch (signal) {
  case stream_signal::pending:
    return "pending";
  }
  return "pending";
}

template <typename char_t, typename traits_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream,
                const stream_signal signal)
    -> std::basic_ostream<char_t, traits_t> & {
  stream << to_string(signal);
  return stream;
}

inline constexpr auto stream_pending = stream_signal::pending;

template <typename value_t>
using stream_result = wh::core::result<value_t>;

template <typename value_t>
using stream_try_result = std::variant<stream_signal, stream_result<value_t>>;

} // namespace wh::schema::stream
