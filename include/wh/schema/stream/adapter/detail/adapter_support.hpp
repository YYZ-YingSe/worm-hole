// Defines shared adapter helpers for stream readers that wrap other readers.
#pragma once

#include <concepts>

#include "wh/core/result.hpp"

namespace wh::schema::stream {

struct auto_close_options;

namespace detail {

template <typename reader_t>
concept auto_close_settable =
    requires(reader_t &reader, const auto_close_options &options) {
      reader.set_automatic_close(options);
    };

template <typename reader_t>
concept source_closed_queryable = requires(const reader_t &reader) {
  { reader.is_source_closed() } -> std::same_as<bool>;
};

template <typename reader_t>
inline auto set_automatic_close_if_supported(reader_t &reader,
                                             const auto_close_options &options)
    -> void {
  if constexpr (auto_close_settable<reader_t>) {
    reader.set_automatic_close(options);
  }
}

template <typename reader_t>
[[nodiscard]] inline auto
is_source_closed_if_supported(const reader_t &reader) noexcept -> bool {
  if constexpr (source_closed_queryable<reader_t>) {
    return reader.is_source_closed();
  }
  return reader.is_closed();
}

template <typename chunk_t>
[[nodiscard]] inline auto make_error_chunk(const wh::core::error_code error)
    -> chunk_t {
  chunk_t chunk{};
  chunk.error = error;
  return chunk;
}

struct stream_adapter_state {
  bool automatic_close{true};
  bool terminal{false};
  bool source_closed{false};

  template <typename reader_t>
  auto close_source_if_enabled(reader_t &reader) -> void {
    if (automatic_close) {
      [[maybe_unused]] const auto close_status = close_source(reader);
    }
  }

  template <typename reader_t>
  auto close_source(reader_t &reader) -> wh::core::result<void> {
    if (source_closed) {
      return {};
    }
    source_closed = true;
    auto status = reader.close();
    if (status.has_error() &&
        status.error() != wh::core::errc::channel_closed) {
      return status;
    }
    return {};
  }
};

} // namespace detail
} // namespace wh::schema::stream
