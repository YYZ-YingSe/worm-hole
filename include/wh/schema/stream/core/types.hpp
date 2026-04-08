// Defines common stream result/status types shared by schema stream reader
// and writer APIs.
#pragma once

#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/core/error.hpp"

namespace wh::schema::stream {

/// Configures whether adapters auto-close upstream readers.
struct auto_close_options {
  /// True means adapters close upstream reader on terminal states.
  bool enabled{true};
};

/// Default option: enable automatic upstream close.
inline constexpr auto_close_options auto_close_enabled{};
/// Default option: disable automatic upstream close.
inline constexpr auto_close_options auto_close_disabled{false};

template <typename value_t> struct stream_chunk {
  /// Owned payload value when present.
  std::optional<value_t> value{};
  /// Non-zero means this chunk carries an error status.
  wh::core::error_code error{};
  /// True marks stream EOF.
  bool eof{false};
  /// Optional source label for merge/select outputs.
  std::string source{};

  template <typename value_u>
    requires std::constructible_from<value_t, value_u &&>
  /// Creates a value chunk.
  [[nodiscard]] static auto make_value(value_u &&value) -> stream_chunk {
    stream_chunk chunk{};
    chunk.value.emplace(std::forward<value_u>(value));
    return chunk;
  }

  /// Creates an EOF chunk.
  [[nodiscard]] static auto make_eof() -> stream_chunk {
    stream_chunk chunk{};
    chunk.eof = true;
    return chunk;
  }

  template <typename source_name_t>
    requires std::constructible_from<std::string, source_name_t &&>
  /// Creates source-tagged EOF chunk.
  [[nodiscard]] static auto make_source_eof(source_name_t &&source_name)
      -> stream_chunk {
    stream_chunk chunk{};
    chunk.eof = true;
    chunk.source = std::forward<source_name_t>(source_name);
    return chunk;
  }

  /// Returns true only for the terminal EOF of the whole stream.
  [[nodiscard]] auto is_terminal_eof() const noexcept -> bool {
    return eof && source.empty();
  }

  /// Returns true for per-source EOF emitted by merged/select streams.
  [[nodiscard]] auto is_source_eof() const noexcept -> bool {
    return eof && !source.empty();
  }
};

template <typename value_t> struct stream_chunk_view {
  /// Borrowed payload pointer valid within caller-defined boundary.
  const value_t *value{nullptr};
  /// Non-zero means this chunk carries an error status.
  wh::core::error_code error{};
  /// True marks stream EOF.
  bool eof{false};
  /// Optional source label borrowing from owner storage.
  std::string_view source{};

  /// Creates a borrowed-value chunk view.
  [[nodiscard]] static auto make_value(const value_t &value_ref)
      -> stream_chunk_view {
    stream_chunk_view chunk{};
    chunk.value = std::addressof(value_ref);
    return chunk;
  }

  /// Creates an EOF chunk view.
  [[nodiscard]] static auto make_eof() -> stream_chunk_view {
    stream_chunk_view chunk{};
    chunk.eof = true;
    return chunk;
  }

  /// Returns true only for the terminal EOF of the whole stream.
  [[nodiscard]] auto is_terminal_eof() const noexcept -> bool {
    return eof && source.empty();
  }

  /// Returns true for per-source EOF emitted by merged/select streams.
  [[nodiscard]] auto is_source_eof() const noexcept -> bool {
    return eof && !source.empty();
  }
};

/// Borrows view from owned chunk until next read/materialization boundary.
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

/// Materializes owned chunk from borrowed chunk view.
template <typename value_t>
[[nodiscard]] inline auto
materialize_chunk(const stream_chunk_view<value_t> &view)
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
