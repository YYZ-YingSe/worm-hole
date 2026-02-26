#pragma once

#include <cstddef>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/stream/pipe_stream.hpp"

namespace wh::schema::stream {

template <typename value_t> struct named_stream_reader {
  std::string source{};
  pipe_stream_reader<value_t> reader{};
  bool finished{false};
};

template <typename value_t>
class named_merge_stream final
    : public stream_base<named_merge_stream<value_t>, value_t> {
public:
  using chunk_type = stream_chunk<value_t>;
  enum class select_mode {
    fixed,
    dynamic,
  };

  named_merge_stream() = default;
  explicit named_merge_stream(std::vector<named_stream_reader<value_t>> readers)
      : readers_(std::move(readers)),
        mode_(readers_.size() <= 5U ? select_mode::fixed
                                    : select_mode::dynamic),
        active_readers_(static_cast<std::size_t>(
            std::ranges::count_if(
                readers_, [](const named_stream_reader<value_t> &reader)
                              -> bool { return !reader.finished; }))) {}

  [[nodiscard]] auto uses_fixed_select_path() const noexcept -> bool {
    return mode_ == select_mode::fixed;
  }

  [[nodiscard]] auto next_impl() -> wh::core::result<chunk_type> {
    if (closed_) {
      return wh::core::result<chunk_type>::failure(
          wh::core::errc::channel_closed);
    }
    if (readers_.empty()) {
      return chunk_type::make_eof();
    }
    if (active_readers_ == 0U) {
      return chunk_type::make_eof();
    }

    if (mode_ == select_mode::fixed) {
      return next_fixed_path();
    }
    return next_dynamic_path();
  }

  auto close_impl() -> wh::core::result<void> {
    for (auto &reader : readers_) {
      if (!reader.finished) {
        auto closed = reader.reader.close();
        if (closed.has_error() &&
            closed.error() != wh::core::errc::channel_closed) {
          return closed;
        }
      }
      reader.finished = true;
    }
    active_readers_ = 0U;
    closed_ = true;
    return {};
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool { return closed_; }

  auto set_automatic_close(const auto_close_options options) -> void {
    automatic_close_ = options.enabled;
    for (auto &reader : readers_) {
      reader.reader.set_automatic_close(options);
    }
  }

private:
  auto poll_one_reader(const std::size_t index,
                       wh::core::result<chunk_type> &output) -> bool {
    auto &selected = readers_[index];
    if (selected.finished) {
      return false;
    }

    auto next_chunk = selected.reader.try_read();
    if (next_chunk.has_error()) {
      if (next_chunk.error() == wh::core::errc::queue_empty) {
        return false;
      }
      output = wh::core::result<chunk_type>::failure(next_chunk.error());
      return true;
    }

    auto chunk = std::move(next_chunk).value();
    if (chunk.eof) {
      if (!selected.finished) {
        selected.finished = true;
        if (active_readers_ > 0U) {
          --active_readers_;
        }
      }
      if (automatic_close_) {
        static_cast<void>(selected.reader.close());
      }
      output = chunk_type::make_source_eof(selected.source);
      return true;
    }
    chunk.source = selected.source;
    output = std::move(chunk);
    return true;
  }

  template <std::size_t width>
  [[nodiscard]] auto next_fixed_width() -> wh::core::result<chunk_type> {
    static_assert(width >= 1U && width <= 5U);
    for (std::size_t attempts = 0U; attempts < width; ++attempts) {
      const auto index = cursor_ % width;
      ++cursor_;
      wh::core::result<chunk_type> output{};
      if (poll_one_reader(index, output)) {
        return output;
      }
    }
    return wh::core::result<chunk_type>::failure(wh::core::errc::queue_empty);
  }

  [[nodiscard]] auto next_fixed_path() -> wh::core::result<chunk_type> {
    switch (readers_.size()) {
    case 0U:
      return chunk_type::make_eof();
    case 1U:
      return next_fixed_width<1U>();
    case 2U:
      return next_fixed_width<2U>();
    case 3U:
      return next_fixed_width<3U>();
    case 4U:
      return next_fixed_width<4U>();
    case 5U:
      return next_fixed_width<5U>();
    default:
      return next_dynamic_path();
    }
  }

  [[nodiscard]] auto next_dynamic_path() -> wh::core::result<chunk_type> {
    const auto reader_count = readers_.size();
    for (std::size_t attempts = 0U; attempts < reader_count; ++attempts) {
      const auto index = cursor_ % reader_count;
      ++cursor_;
      wh::core::result<chunk_type> output{};
      if (poll_one_reader(index, output)) {
        return output;
      }
    }
    return wh::core::result<chunk_type>::failure(wh::core::errc::queue_empty);
  }

  std::vector<named_stream_reader<value_t>> readers_{};
  select_mode mode_{select_mode::fixed};
  std::size_t active_readers_{0U};
  std::size_t cursor_{0U};
  bool closed_{false};
  bool automatic_close_{true};
};

template <typename value_t>
[[nodiscard]] inline auto
merge_named(std::vector<named_stream_reader<value_t>> readers)
    -> named_merge_stream<value_t> {
  std::ranges::sort(readers, [](const named_stream_reader<value_t> &left,
                                const named_stream_reader<value_t> &right) {
    return left.source < right.source;
  });
  return named_merge_stream<value_t>{std::move(readers)};
}

template <typename value_t>
[[nodiscard]] inline auto
merge(std::vector<pipe_stream_reader<value_t>> readers)
    -> named_merge_stream<value_t> {
  std::vector<named_stream_reader<value_t>> named_readers{};
  named_readers.reserve(readers.size());
  for (std::size_t index = 0U; index < readers.size(); ++index) {
    named_readers.push_back(named_stream_reader<value_t>{
        std::to_string(index), std::move(readers[index]), false});
  }
  return merge_named<value_t>(std::move(named_readers));
}

} // namespace wh::schema::stream
