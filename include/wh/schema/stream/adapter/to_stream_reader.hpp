// Defines adapters that wrap plain reader values into stream chunks.
#pragma once

#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/schema/stream/adapter/detail/adapter_support.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::schema::stream {

template <stream_reader reader_t>
class to_stream_reader final
    : public stream_base<to_stream_reader<reader_t>,
                         typename reader_t::value_type> {
public:
  using value_type = typename reader_t::value_type;
  using chunk_type = stream_chunk<value_type>;

  explicit to_stream_reader(reader_t reader) : reader_(std::move(reader)) {}

  [[nodiscard]] auto read_impl() -> stream_result<chunk_type> {
    if (state_.terminal) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }

    stream_result<chunk_type> next{};
    try {
      next = reader_.read();
    } catch (...) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      return stream_result<chunk_type>{detail::make_error_chunk<chunk_type>(
          wh::core::map_current_exception())};
    }
    return map_read_chunk(std::move(next));
  }

  [[nodiscard]] auto try_read_impl() -> stream_try_result<chunk_type> {
    if (state_.terminal) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }

    stream_try_result<chunk_type> next{};
    try {
      next = reader_.try_read();
    } catch (...) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      return stream_result<chunk_type>{detail::make_error_chunk<chunk_type>(
          wh::core::map_current_exception())};
    }
    return map_try_chunk(std::move(next));
  }

  [[nodiscard]] auto read_async() const
    requires detail::async_stream_reader<reader_t>
  {
    using input_result_t = stream_result<chunk_type>;
    return reader_.read_async() | stdexec::then([](auto status) {
             return input_result_t{std::move(status)};
           }) |
           stdexec::upon_error([](auto &&) noexcept {
             return input_result_t::failure(wh::core::errc::internal_error);
           }) |
           stdexec::then([this](input_result_t next) {
             return map_read_chunk(std::move(next));
           });
  }

  auto close_impl() -> wh::core::result<void> {
    state_.terminal = true;
    return state_.close_source(reader_);
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return state_.terminal || reader_.is_closed();
  }

  auto set_automatic_close(const auto_close_options &options) -> void {
    state_.automatic_close = options.enabled;
    detail::set_automatic_close_if_supported(reader_, options);
  }

private:
  [[nodiscard]] auto map_read_chunk(stream_result<chunk_type> next) const
      -> stream_result<chunk_type> {
    if (next.has_error()) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      return stream_result<chunk_type>::failure(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.is_terminal_eof() || chunk.error.failed()) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
    }
    return stream_result<chunk_type>{std::move(chunk)};
  }

  [[nodiscard]] auto map_try_chunk(stream_try_result<chunk_type> next) const
      -> stream_try_result<chunk_type> {
    if (std::holds_alternative<stream_signal>(next)) {
      return stream_pending;
    }
    return map_read_chunk(std::move(std::get<stream_result<chunk_type>>(next)));
  }

  mutable reader_t reader_;
  mutable detail::stream_adapter_state state_{};
};

template <typename reader_t>
  requires stream_reader<std::remove_cvref_t<reader_t>>
[[nodiscard]] inline auto make_to_stream_reader(reader_t &&reader)
    -> to_stream_reader<std::remove_cvref_t<reader_t>> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  return to_stream_reader<stored_reader_t>{
      stored_reader_t{std::forward<reader_t>(reader)}};
}

} // namespace wh::schema::stream
