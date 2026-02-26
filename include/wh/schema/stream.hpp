#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "wh/core/error_domain.hpp"
#include "wh/core/type_utils.hpp"
#include "wh/schema/select.hpp"
#include "wh/schema/stream/pipe_stream.hpp"
#include "wh/schema/stream/stream_base.hpp"
#include "wh/schema/stream/types.hpp"

namespace wh::schema::stream {

template <typename reader_t>
concept stream_reader_like = requires(reader_t reader) {
  typename reader_t::value_type;
  reader.try_read();
  reader.try_read_borrowed_until_next();
  reader.close();
  reader.is_closed();
};

namespace detail {

template <typename reader_t>
concept auto_close_settable =
    requires(reader_t &reader, const auto_close_options options) {
      reader.set_automatic_close(options);
};

template <typename reader_t>
inline auto set_automatic_close_if_supported(reader_t &reader,
                                             const auto_close_options options)
    -> void {
  if constexpr (auto_close_settable<reader_t>) {
    reader.set_automatic_close(options);
  }
}

template <typename chunk_t>
[[nodiscard]] inline auto make_error_chunk(const wh::core::error_code error)
    -> wh::core::result<chunk_t> {
  chunk_t chunk{};
  chunk.error = error;
  return chunk;
}

} // namespace detail

template <stream_reader_like reader_t>
class copied_stream_reader final
    : public stream_base<copied_stream_reader<reader_t>,
                         typename reader_t::value_type> {
private:
  struct shared_state;

public:
  using value_type = typename reader_t::value_type;
  using chunk_type = stream_chunk<value_type>;

  copied_stream_reader() = default;
  copied_stream_reader(std::shared_ptr<shared_state> state,
                       const std::size_t reader_index)
      : state_(std::move(state)), reader_index_(reader_index) {}

  [[nodiscard]] static auto make_copies(reader_t reader, std::size_t count)
      -> std::vector<copied_stream_reader>
    requires std::copy_constructible<value_type>
  {
    if (count == 0U) {
      return {};
    }

    const auto reader_count = count < 2U ? 1U : count;
    auto state = std::make_shared<shared_state>(std::move(reader), reader_count);
    std::vector<copied_stream_reader> copies{};
    copies.reserve(reader_count);
    for (std::size_t index = 0U; index < reader_count; ++index) {
      copies.emplace_back(state, index);
    }
    return copies;
  }

  [[nodiscard]] auto next_impl() -> wh::core::result<chunk_type> {
    if (!state_) {
      return wh::core::result<chunk_type>::failure(wh::core::errc::not_found);
    }
    return state_->next_for(reader_index_);
  }

  auto close_impl() -> wh::core::result<void> {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return state_->close_reader(reader_index_);
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return !state_ || state_->is_reader_closed(reader_index_);
  }

  auto set_automatic_close(const auto_close_options options) -> void {
    if (state_) {
      state_->set_automatic_close(options);
    }
  }

private:
  struct shared_state {
    explicit shared_state(reader_t source, const std::size_t reader_count)
        : source_(std::move(source)), cursors_(reader_count, 0U),
          closed_(reader_count, 0U), open_reader_count_(reader_count) {}

    [[nodiscard]] auto next_for(const std::size_t reader_index)
        -> wh::core::result<chunk_type> {
      if (reader_index >= closed_.size()) {
        return wh::core::result<chunk_type>::failure(
            wh::core::errc::invalid_argument);
      }
      if (closed_[reader_index]) {
        return wh::core::result<chunk_type>::failure(
            wh::core::errc::channel_closed);
      }

      if (cursors_[reader_index] < offset_ + buffered_size()) {
        return consume_buffered(reader_index);
      }

      auto fetched = fetch_one();
      if (fetched.has_error() && fetched.error() == wh::core::errc::queue_empty) {
        return fetched;
      }
      if (cursors_[reader_index] < offset_ + buffered_size()) {
        return consume_buffered(reader_index);
      }
      return fetched;
    }

    auto close_reader(const std::size_t reader_index) -> wh::core::result<void> {
      if (reader_index >= closed_.size()) {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
      if (closed_[reader_index]) {
        return {};
      }

      closed_[reader_index] = 1U;
      if (open_reader_count_ > 0U) {
        --open_reader_count_;
      }

      if (open_reader_count_ == 0U) {
        if (automatic_close_) {
          auto close_status = source_.close();
          if (close_status.has_error() &&
              close_status.error() != wh::core::errc::channel_closed) {
            return wh::core::result<void>::failure(close_status.error());
          }
        }
      }
      compact_prefix();
      return {};
    }

    auto set_automatic_close(const auto_close_options options) -> void {
      automatic_close_ = options.enabled;
      detail::set_automatic_close_if_supported(source_, options);
    }

    [[nodiscard]] auto is_reader_closed(const std::size_t reader_index) const
        -> bool {
      if (reader_index >= closed_.size()) {
        return true;
      }
      return closed_[reader_index] != 0U;
    }

  private:
    [[nodiscard]] auto fetch_one() -> wh::core::result<chunk_type> {
      if (source_terminal_) {
        return wh::core::result<chunk_type>::failure(
            wh::core::errc::channel_closed);
      }

      auto next = source_.try_read();
      if (next.has_error()) {
        if (next.error() == wh::core::errc::queue_empty) {
          return next;
        }
        events_.push_back(wh::core::result<chunk_type>::failure(next.error()));
        source_terminal_ = true;
        return events_.back();
      }

      events_.push_back(std::move(next).value());
      if (events_.back().has_value() && events_.back().value().eof) {
        source_terminal_ = true;
      }
      return events_.back();
    }

    [[nodiscard]] auto consume_buffered(const std::size_t reader_index)
        -> wh::core::result<chunk_type> {
      const auto event_index =
          front_index_ + (cursors_[reader_index] - offset_);
      auto status = events_[event_index];
      ++cursors_[reader_index];
      compact_prefix();
      return status;
    }

    auto compact_prefix() -> void {
      if (events_.empty()) {
        return;
      }

      bool has_open_reader = false;
      std::size_t min_cursor = std::numeric_limits<std::size_t>::max();
      for (std::size_t index = 0U; index < closed_.size(); ++index) {
        if (closed_[index] != 0U) {
          continue;
        }
        has_open_reader = true;
        min_cursor = std::min(min_cursor, cursors_[index]);
      }

      if (!has_open_reader) {
        events_.clear();
        offset_ = 0U;
        front_index_ = 0U;
        return;
      }
      if (min_cursor <= offset_) {
        return;
      }

      auto erase_count = min_cursor - offset_;
      if (erase_count > buffered_size()) {
        erase_count = buffered_size();
      }
      if (erase_count == 0U) {
        return;
      }

      offset_ += erase_count;
      front_index_ += erase_count;
      if (front_index_ == events_.size()) {
        events_.clear();
        front_index_ = 0U;
        return;
      }

      constexpr std::size_t compact_threshold = 64U;
      if (front_index_ >= compact_threshold &&
          front_index_ * 2U >= events_.size()) {
        events_.erase(events_.begin(),
                      events_.begin() +
                          static_cast<std::ptrdiff_t>(front_index_));
        front_index_ = 0U;
      }
    }

    [[nodiscard]] auto buffered_size() const noexcept -> std::size_t {
      return events_.size() - front_index_;
    }

    reader_t source_;
    std::vector<wh::core::result<chunk_type>> events_{};
    std::size_t front_index_{0U};
    std::vector<std::size_t> cursors_{};
    std::vector<std::uint8_t> closed_{};
    std::size_t open_reader_count_{0U};
    std::size_t offset_{0U};
    bool source_terminal_{false};
    bool automatic_close_{true};
  };

  std::shared_ptr<shared_state> state_{};
  std::size_t reader_index_{0U};
};

template <stream_reader_like reader_t>
using copy_readers_result = std::variant<reader_t, std::vector<copied_stream_reader<reader_t>>>;

template <stream_reader_like reader_t>
[[nodiscard]] inline auto copy_readers(reader_t reader, const std::size_t count)
    -> copy_readers_result<reader_t>
  requires std::copy_constructible<typename reader_t::value_type>
{
  if (count < 2U) {
    return std::move(reader);
  }
  return copied_stream_reader<reader_t>::make_copies(std::move(reader), count);
}

template <stream_reader_like reader_t, typename transform_t>
class converted_stream_reader final
    : public stream_base<
          converted_stream_reader<reader_t, transform_t>,
          typename wh::core::remove_cvref_t<std::invoke_result_t<
              transform_t &, const typename reader_t::value_type &>>::value_type> {
private:
  using input_value_t = typename reader_t::value_type;
  using transform_result_t =
      wh::core::remove_cvref_t<std::invoke_result_t<transform_t &,
                                                    const input_value_t &>>;

public:
  static_assert(wh::core::is_result_v<transform_result_t>,
                "convert transform must return wh::core::result<T>");
  static_assert(!std::same_as<typename transform_result_t::value_type, void>,
                "convert transform result value_type cannot be void");

  using value_type = typename transform_result_t::value_type;
  using input_chunk_type = stream_chunk<input_value_t>;
  using input_chunk_view_type = stream_chunk_view<input_value_t>;
  using chunk_type = stream_chunk<value_type>;

  converted_stream_reader(reader_t reader, transform_t transform)
      : reader_(std::move(reader)), transform_(std::move(transform)) {}

  [[nodiscard]] auto next_impl() -> wh::core::result<chunk_type> {
    while (true) {
      wh::core::result<input_chunk_view_type> next{};
      try {
        next = reader_.try_read_borrowed_until_next();
      } catch (...) {
        terminal_ = true;
        close_source_if_enabled();
        return detail::make_error_chunk<chunk_type>(
            wh::core::map_current_exception());
      }

      if (next.has_error()) {
        if (next.error() == wh::core::errc::queue_empty) {
          return wh::core::result<chunk_type>::failure(
              wh::core::errc::queue_empty);
        }
        terminal_ = true;
        close_source_if_enabled();
        return detail::make_error_chunk<chunk_type>(
            next.error());
      }

      const auto input_chunk = next.value();
      if (input_chunk.eof) {
        terminal_ = true;
        close_source_if_enabled();
        auto eof_chunk = chunk_type::make_eof();
        eof_chunk.source = std::string{input_chunk.source};
        return eof_chunk;
      }
      if (input_chunk.error.failed()) {
        terminal_ = true;
        close_source_if_enabled();
        auto output_chunk = chunk_type{};
        output_chunk.source = std::string{input_chunk.source};
        output_chunk.error = input_chunk.error;
        return output_chunk;
      }
      if (input_chunk.value == nullptr) {
        terminal_ = true;
        close_source_if_enabled();
        return wh::core::result<chunk_type>::failure(
            wh::core::errc::protocol_error);
      }

      transform_result_t converted{};
      try {
        converted = transform_(*input_chunk.value);
      } catch (...) {
        terminal_ = true;
        close_source_if_enabled();
        return detail::make_error_chunk<chunk_type>(
            wh::core::map_current_exception());
      }

      if (converted.has_error()) {
        if (converted.error() == wh::core::errc::queue_empty) {
          continue;
        }
        terminal_ = true;
        close_source_if_enabled();
        return wh::core::result<chunk_type>::failure(converted.error());
      }

      auto output_chunk = chunk_type::make_value(std::move(converted).value());
      output_chunk.source = std::string{input_chunk.source};
      return output_chunk;
    }
  }

  auto close_impl() -> wh::core::result<void> {
    terminal_ = true;
    return close_source();
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return terminal_ || reader_.is_closed();
  }

  auto set_automatic_close(const auto_close_options options) -> void {
    automatic_close_ = options.enabled;
    detail::set_automatic_close_if_supported(reader_, options);
  }

private:
  auto close_source_if_enabled() -> void {
    if (automatic_close_) {
      static_cast<void>(close_source());
    }
  }

  auto close_source() -> wh::core::result<void> {
    if (source_closed_) {
      return {};
    }
    source_closed_ = true;
    auto status = reader_.close();
    if (status.has_error() &&
        status.error() != wh::core::errc::channel_closed) {
      return status;
    }
    return {};
  }

  reader_t reader_;
  transform_t transform_;
  bool automatic_close_{true};
  bool terminal_{false};
  bool source_closed_{false};
};

template <stream_reader_like reader_t, typename transform_t>
[[nodiscard]] inline auto convert(reader_t reader, transform_t transform)
    -> converted_stream_reader<reader_t, transform_t> {
  return converted_stream_reader<reader_t, transform_t>{std::move(reader),
                                                        std::move(transform)};
}

template <stream_reader_like reader_t>
class to_stream_reader final
    : public stream_base<to_stream_reader<reader_t>,
                         typename reader_t::value_type> {
public:
  using value_type = typename reader_t::value_type;
  using chunk_type = stream_chunk<value_type>;

  explicit to_stream_reader(reader_t reader) : reader_(std::move(reader)) {}

  [[nodiscard]] auto next_impl() -> wh::core::result<chunk_type> {
    if (terminal_) {
      return wh::core::result<chunk_type>::failure(
          wh::core::errc::channel_closed);
    }

    wh::core::result<chunk_type> next{};
    try {
      next = reader_.try_read();
    } catch (...) {
      terminal_ = true;
      close_source_if_enabled();
      return detail::make_error_chunk<chunk_type>(
          wh::core::map_current_exception());
    }

    if (next.has_error()) {
      if (next.error() == wh::core::errc::queue_empty) {
        return wh::core::result<chunk_type>::failure(
            wh::core::errc::queue_empty);
      }
      if (next.error() == wh::core::errc::channel_closed) {
        terminal_ = true;
        close_source_if_enabled();
        return chunk_type::make_eof();
      }
      terminal_ = true;
      close_source_if_enabled();
      return detail::make_error_chunk<chunk_type>(
          next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.eof || chunk.error.failed()) {
      terminal_ = true;
      close_source_if_enabled();
    }
    return chunk;
  }

  auto close_impl() -> wh::core::result<void> {
    terminal_ = true;
    return close_source();
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return terminal_ || reader_.is_closed();
  }

  auto set_automatic_close(const auto_close_options options) -> void {
    automatic_close_ = options.enabled;
    detail::set_automatic_close_if_supported(reader_, options);
  }

private:
  auto close_source_if_enabled() -> void {
    if (automatic_close_) {
      static_cast<void>(close_source());
    }
  }

  auto close_source() -> wh::core::result<void> {
    if (source_closed_) {
      return {};
    }
    source_closed_ = true;
    auto status = reader_.close();
    if (status.has_error() &&
        status.error() != wh::core::errc::channel_closed) {
      return status;
    }
    return {};
  }

  reader_t reader_;
  bool automatic_close_{true};
  bool terminal_{false};
  bool source_closed_{false};
};

template <stream_reader_like reader_t>
[[nodiscard]] inline auto to_stream(reader_t reader) -> to_stream_reader<reader_t> {
  return to_stream_reader<reader_t>{std::move(reader)};
}

template <typename value_t>
[[nodiscard]] inline auto
select(std::vector<pipe_stream_reader<value_t>> readers)
    -> named_merge_stream<value_t> {
  return merge<value_t>(std::move(readers));
}

} // namespace wh::schema::stream
