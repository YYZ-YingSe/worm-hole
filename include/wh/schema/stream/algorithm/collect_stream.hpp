// Defines helpers that materialize whole streams into owned values.
#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/core/result.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/status.hpp"

namespace wh::schema::stream {

namespace detail {

template <stream_reader reader_t, typename on_value_t>
inline auto drain_stream(reader_t &reader, on_value_t &&on_value) -> wh::core::result<void> {
  while (true) {
    auto next = reader.read();
    if (next.has_error()) {
      return wh::core::result<void>::failure(next.error());
    }
    auto chunk = std::move(next).value();
    if (chunk.is_terminal_eof()) {
      break;
    }
    if (chunk.is_source_eof()) {
      continue;
    }
    if (chunk.error != wh::core::errc::ok) {
      return wh::core::result<void>::failure(chunk.error);
    }
    if (chunk.value.has_value()) {
      auto status = on_value(std::move(*chunk.value));
      if (status.has_error()) {
        return status;
      }
    }
  }
  return {};
}

} // namespace detail

template <stream_reader reader_t>
[[nodiscard]] inline auto collect_stream_reader(reader_t &&reader)
    -> wh::core::result<std::vector<typename std::remove_cvref_t<reader_t>::value_type>> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  using value_t = typename stored_reader_t::value_type;

  stored_reader_t owned_reader{std::forward<reader_t>(reader)};
  std::vector<value_t> values{};
  auto drained = detail::drain_stream(owned_reader, [&values](value_t value) {
    values.push_back(std::move(value));
    return wh::core::result<void>{};
  });
  if (drained.has_error()) {
    return wh::core::result<std::vector<value_t>>::failure(drained.error());
  }
  auto closed = owned_reader.close();
  if (closed.has_error()) {
    return wh::core::result<std::vector<value_t>>::failure(closed.error());
  }
  return values;
}

template <stream_reader reader_t>
  requires std::same_as<typename std::remove_cvref_t<reader_t>::value_type, std::string>
[[nodiscard]] inline auto collect_text_stream_reader(reader_t &&reader)
    -> wh::core::result<std::string> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  stored_reader_t owned_reader{std::forward<reader_t>(reader)};
  std::string output{};
  auto drained = detail::drain_stream(owned_reader, [&output](std::string value) {
    output.append(value);
    return wh::core::result<void>{};
  });
  if (drained.has_error()) {
    return wh::core::result<std::string>::failure(drained.error());
  }
  auto closed = owned_reader.close();
  if (closed.has_error()) {
    return wh::core::result<std::string>::failure(closed.error());
  }
  return output;
}

} // namespace wh::schema::stream
