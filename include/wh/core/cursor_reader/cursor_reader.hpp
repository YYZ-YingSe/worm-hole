#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/core/cursor_reader/detail/read_sender.hpp"
#include "wh/core/cursor_reader/detail/source.hpp"

namespace wh::core {

/// Fixed-cardinality retained fanout reader core.
///
/// One source is read once, each emitted result is retained until the slowest
/// reader cursor advances past it. Readers are created up front and do not
/// support runtime fork/copy.
template <cursor_reader_source source_t,
          typename policy_t = cursor_reader_detail::default_policy<source_t>>
  requires cursor_reader_detail::policy_for<source_t, policy_t>
class cursor_reader {
private:
  using shared_state_t = cursor_reader_detail::shared_state<source_t, policy_t>;
  using result_type = typename policy_t::result_type;
  using try_result_type = typename policy_t::try_result_type;

public:
  using source_reader_type = source_t;
  using read_result_type = result_type;
  using try_read_result_type = try_result_type;
  using policy_type = policy_t;

  /// Creates an empty cursor handle.
  cursor_reader() = default;
  cursor_reader(const cursor_reader &) = delete;
  auto operator=(const cursor_reader &) -> cursor_reader & = delete;

  /// Moves one pre-created cursor handle.
  cursor_reader(cursor_reader &&other) noexcept
      : state_(std::move(other.state_)), reader_index_(other.reader_index_),
        released_(other.released_) {
    other.released_ = true;
  }

  /// Transfers cursor ownership.
  auto operator=(cursor_reader &&other) noexcept -> cursor_reader & {
    if (this == &other) {
      return *this;
    }
    release_reader();
    state_ = std::move(other.state_);
    reader_index_ = other.reader_index_;
    released_ = other.released_;
    other.released_ = true;
    return *this;
  }

  /// Releases the cursor handle.
  ~cursor_reader() { release_reader(); }

  /// Creates a fixed set of cursor readers from one source.
  template <typename source_u>
    requires std::constructible_from<source_t, source_u &&> &&
             std::copy_constructible<result_type>
  [[nodiscard]] static auto make_readers(source_u &&source,
                                         const std::size_t count)
      -> std::vector<cursor_reader> {
    if (count == 0U) {
      return {};
    }

    auto state = std::make_shared<shared_state_t>(
        source_t{std::forward<source_u>(source)}, count);
    std::vector<cursor_reader> readers{};
    readers.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
      cursor_reader current{};
      current.state_ = state;
      current.reader_index_ = index;
      current.released_ = false;
      readers.push_back(std::move(current));
    }
    return readers;
  }

  /// Tries to read one retained result without waiting.
  [[nodiscard]] auto try_read() -> try_result_type {
    if (!state_) {
      return policy_t::ready(policy_t::internal_result());
    }
    if (released_) {
      return policy_t::ready(policy_t::closed_result());
    }
    return state_->try_read_for(reader_index_);
  }

  /// Reads one retained result, waiting when necessary.
  [[nodiscard]] auto read() -> result_type {
    if (!state_) {
      return policy_t::internal_result();
    }
    if (released_) {
      return policy_t::closed_result();
    }
    return state_->read_for(reader_index_);
  }

  /// Reads one retained result asynchronously.
  [[nodiscard]] auto read_async() const
    requires cursor_reader_detail::async_source<source_t>
  {
    return cursor_reader_detail::read_sender<source_t, policy_t>{
        state_, reader_index_, released_};
  }

  /// Closes this cursor only.
  auto close() -> wh::core::result<void> {
    if (!state_ || released_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    released_ = true;
    state_->close_reader(reader_index_);
    return {};
  }

  /// Returns whether this cursor is closed.
  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return !state_ || released_ || state_->reader_is_closed(reader_index_);
  }

  /// Returns whether the shared source has reached terminal state.
  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    return !state_ || state_->is_source_closed();
  }

  /// Controls whether closing the last cursor closes the source.
  auto set_automatic_close(const bool enabled) -> void {
    if (state_) {
      state_->set_automatic_close(enabled);
    }
  }

private:
  auto release_reader() noexcept -> void {
    if (!state_ || released_) {
      return;
    }
    released_ = true;
    state_->close_reader(reader_index_);
  }

  std::shared_ptr<shared_state_t> state_{};
  std::size_t reader_index_{0U};
  bool released_{true};
};

template <cursor_reader_source source_t,
          typename policy_t = cursor_reader_detail::default_policy<source_t>>
  requires cursor_reader_detail::policy_for<source_t, policy_t>
[[nodiscard]] inline auto make_cursor_readers(source_t &&source,
                                              const std::size_t count)
    -> std::vector<cursor_reader<std::remove_cvref_t<source_t>, policy_t>>
  requires std::copy_constructible<
      cursor_reader_result_t<std::remove_cvref_t<source_t>>>
{
  return cursor_reader<std::remove_cvref_t<source_t>, policy_t>::make_readers(
      std::forward<source_t>(source), count);
}

} // namespace wh::core
