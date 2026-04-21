#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>

namespace wh::core {

enum class bounded_queue_status : std::uint8_t {
  success = 0U,
  empty,
  full,
  closed,
  busy,
  busy_async,
};

[[nodiscard]] constexpr auto to_string(const bounded_queue_status status) noexcept
    -> std::string_view {
  switch (status) {
  case bounded_queue_status::success:
    return "success";
  case bounded_queue_status::empty:
    return "empty";
  case bounded_queue_status::full:
    return "full";
  case bounded_queue_status::closed:
    return "closed";
  case bounded_queue_status::busy:
    return "busy";
  case bounded_queue_status::busy_async:
    return "busy_async";
  }

  return "unknown";
}

template <typename char_t, typename traits_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream, const bounded_queue_status status)
    -> std::basic_ostream<char_t, traits_t> & {
  stream << to_string(status);
  return stream;
}

} // namespace wh::core
