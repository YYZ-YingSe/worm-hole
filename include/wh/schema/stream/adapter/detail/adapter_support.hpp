// Defines shared adapter helpers for stream readers that wrap other readers.
#pragma once

#include <concepts>
#include <functional>

#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::schema::stream {

struct auto_close_options;

namespace detail {

template <typename reader_t>
concept auto_close_settable = requires(reader_t &reader, const auto_close_options &options) {
  reader.set_automatic_close(options);
};

template <typename reader_t>
concept source_closed_queryable = requires(const reader_t &reader) {
  { reader.is_source_closed() } -> std::same_as<bool>;
};

template <typename reader_t>
inline auto set_automatic_close_if_supported(reader_t &reader, const auto_close_options &options)
    -> void {
  if constexpr (auto_close_settable<reader_t>) {
    reader.set_automatic_close(options);
  }
}

template <typename reader_t>
[[nodiscard]] inline auto is_source_closed_if_supported(const reader_t &reader) noexcept -> bool {
  if constexpr (source_closed_queryable<reader_t>) {
    return reader.is_source_closed();
  }
  return reader.is_closed();
}

template <typename chunk_t>
[[nodiscard]] inline auto make_error_chunk(const wh::core::error_code error) -> chunk_t {
  chunk_t chunk{};
  chunk.error = error;
  return chunk;
}

template <typename callback_t, typename input_value_t>
concept adapter_callback = std::invocable<callback_t &, const input_value_t &> ||
                           std::invocable<callback_t &, input_value_t &&>;

template <typename callback_t, typename input_value_t>
  requires std::invocable<callback_t &, input_value_t &&>
[[nodiscard]] auto select_adapter_result(int)
    -> wh::core::remove_cvref_t<wh::core::callable_result_t<callback_t &, input_value_t &&>>;

template <typename callback_t, typename input_value_t>
  requires(!std::invocable<callback_t &, input_value_t &&> &&
           std::invocable<callback_t &, const input_value_t &>)
[[nodiscard]] auto select_adapter_result(long)
    -> wh::core::remove_cvref_t<wh::core::callable_result_t<callback_t &, const input_value_t &>>;

template <typename callback_t, typename value_t>
[[nodiscard]] decltype(auto) invoke_adapter_callback(callback_t &callback, value_t &&value) {
  if constexpr (std::invocable<callback_t &, value_t>) {
    return std::invoke(callback, std::forward<value_t>(value));
  } else {
    return std::invoke(callback, std::as_const(value));
  }
}

struct stream_adapter_state {
  bool automatic_close{true};
  bool terminal{false};
  bool source_closed{false};

  template <typename reader_t> auto close_source_if_enabled(reader_t &reader) -> void {
    if (automatic_close) {
      [[maybe_unused]] const auto close_status = close_source(reader);
    }
  }

  template <typename reader_t> auto close_source(reader_t &reader) -> wh::core::result<void> {
    if (source_closed) {
      return {};
    }
    source_closed = true;
    auto status = reader.close();
    if (status.has_error() && status.error() != wh::core::errc::channel_closed) {
      return status;
    }
    return {};
  }
};

} // namespace detail
} // namespace wh::schema::stream
