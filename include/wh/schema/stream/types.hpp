#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/core/error.hpp"

namespace wh::schema::stream {

struct auto_close_options {
  bool enabled{true};
};

inline constexpr auto_close_options auto_close_enabled{};
inline constexpr auto_close_options auto_close_disabled{false};

template <typename value_t> struct stream_chunk {
  std::optional<value_t> value{};
  wh::core::error_code error{};
  bool eof{false};
  std::string source{};

  [[nodiscard]] static auto make_value(value_t value) -> stream_chunk {
    stream_chunk chunk{};
    chunk.value = std::move(value);
    return chunk;
  }

  [[nodiscard]] static auto make_eof() -> stream_chunk {
    stream_chunk chunk{};
    chunk.eof = true;
    return chunk;
  }

  [[nodiscard]] static auto make_source_eof(std::string source_name)
      -> stream_chunk {
    stream_chunk chunk{};
    chunk.eof = true;
    chunk.source = std::move(source_name);
    return chunk;
  }
};

template <typename value_t> struct stream_chunk_view {
  const value_t *value{nullptr};
  wh::core::error_code error{};
  bool eof{false};
  std::string_view source{};

  [[nodiscard]] static auto make_value(const value_t &value_ref)
      -> stream_chunk_view {
    stream_chunk_view chunk{};
    chunk.value = std::addressof(value_ref);
    return chunk;
  }

  [[nodiscard]] static auto make_eof() -> stream_chunk_view {
    stream_chunk_view chunk{};
    chunk.eof = true;
    return chunk;
  }
};

template <typename value_t>
[[nodiscard]] inline auto
borrow_chunk_until_next(const stream_chunk<value_t> &chunk)
    -> stream_chunk_view<value_t> {
  stream_chunk_view<value_t> view{};
  if (chunk.value.has_value()) {
    view.value = std::addressof(*chunk.value);
  }
  view.error = chunk.error;
  view.eof = chunk.eof;
  view.source = chunk.source;
  return view;
}

template <typename value_t>
[[nodiscard]] inline auto materialize_chunk(const stream_chunk_view<value_t> &view)
    -> stream_chunk<value_t> {
  stream_chunk<value_t> chunk{};
  if (view.value != nullptr) {
    chunk.value = *view.value;
  }
  chunk.error = view.error;
  chunk.eof = view.eof;
  chunk.source = std::string{view.source};
  return chunk;
}

} // namespace wh::schema::stream
