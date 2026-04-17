// Defines an owning move-only reader over a fixed in-memory value range.
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <ranges>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/stream_base.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace wh::schema::stream {

template <typename range_t>
concept values_stream_range = std::ranges::forward_range<range_t> &&
                              std::movable<std::ranges::range_value_t<range_t>>;

template <values_stream_range range_t>
class values_stream_reader final
    : public stream_base<
          values_stream_reader<range_t>,
          std::remove_cvref_t<std::ranges::range_value_t<range_t>>> {
public:
  using value_type = std::remove_cvref_t<std::ranges::range_value_t<range_t>>;
  using chunk_type = stream_chunk<value_type>;
  using iterator_t = std::ranges::iterator_t<range_t>;
  using sentinel_t = std::ranges::sentinel_t<range_t>;

  explicit values_stream_reader(range_t values) noexcept(
      std::is_nothrow_move_constructible_v<range_t>)
      : values_(std::move(values)) {
    restore_cursor();
  }

  values_stream_reader(const values_stream_reader &) = delete;
  auto operator=(const values_stream_reader &)
      -> values_stream_reader & = delete;

  values_stream_reader(values_stream_reader &&other) noexcept(
      std::is_nothrow_move_constructible_v<range_t>)
      : values_(std::move(other.values_)), index_(other.index_),
        closed_(other.closed_) {
    restore_cursor();
    other.close_impl();
  }

  auto operator=(values_stream_reader &&other) noexcept(
      std::is_nothrow_move_assignable_v<range_t> &&
      std::is_nothrow_move_constructible_v<range_t>) -> values_stream_reader & {
    if (this == &other) {
      return *this;
    }
    values_ = std::move(other.values_);
    index_ = other.index_;
    closed_ = other.closed_;
    restore_cursor();
    other.close_impl();
    return *this;
  }

  ~values_stream_reader() = default;

  [[nodiscard]] auto read_impl() -> stream_result<chunk_type> {
    return take_next();
  }

  [[nodiscard]] auto try_read_impl() -> stream_try_result<chunk_type> {
    return take_next();
  }

  [[nodiscard]] auto read_async() & {
    using result_t = stream_result<chunk_type>;
    return wh::core::detail::defer_sender([this]() {
      return wh::core::detail::ready_sender<result_t>(this->read());
    });
  }

  auto close_impl() -> wh::core::result<void> {
    closed_ = true;
    current_ = end_;
    return {};
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool { return closed_; }

  [[nodiscard]] auto is_source_closed() const noexcept -> bool { return true; }

private:
  auto restore_cursor() -> void {
    current_ = std::ranges::begin(values_);
    end_ = std::ranges::end(values_);
    std::ranges::advance(
        current_, static_cast<std::ranges::range_difference_t<range_t>>(index_),
        end_);
  }

  [[nodiscard]] auto take_next() -> stream_result<chunk_type> {
    if (closed_) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }
    if (current_ == end_) {
      closed_ = true;
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }

    auto chunk = chunk_type::make_value(std::move(*current_));
    ++current_;
    ++index_;
    return stream_result<chunk_type>{std::move(chunk)};
  }

  range_t values_;
  iterator_t current_{};
  sentinel_t end_{};
  std::size_t index_{0U};
  bool closed_{false};
};

template <typename value_t>
[[nodiscard]] inline auto make_empty_stream_reader()
    -> values_stream_reader<std::array<value_t, 0U>> {
  return values_stream_reader<std::array<value_t, 0U>>{
      std::array<value_t, 0U>{}};
}

template <typename value_t, typename value_u>
  requires std::constructible_from<value_t, value_u &&>
[[nodiscard]] inline auto make_single_value_stream_reader(value_u &&value)
    -> values_stream_reader<std::array<value_t, 1U>> {
  return values_stream_reader<std::array<value_t, 1U>>{
      std::array<value_t, 1U>{value_t{std::forward<value_u>(value)}}};
}

template <typename range_t>
  requires values_stream_range<std::remove_cvref_t<range_t>> &&
           std::constructible_from<std::remove_cvref_t<range_t>, range_t &&>
[[nodiscard]] inline auto make_values_stream_reader(range_t &&values)
    -> values_stream_reader<std::remove_cvref_t<range_t>> {
  using stored_range_t = std::remove_cvref_t<range_t>;
  return values_stream_reader<stored_range_t>{
      stored_range_t{std::forward<range_t>(values)}};
}

} // namespace wh::schema::stream
