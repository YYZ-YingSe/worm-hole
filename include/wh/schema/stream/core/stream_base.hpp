// Defines base stream interfaces and shared stream behaviors used by
// concrete reader/writer implementations.
#pragma once

#include <optional>

#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace wh::schema::stream {

namespace detail {

template <typename type_t> inline constexpr bool stream_always_false_v = false;

template <typename derived_t>
concept stream_read_impl =
    requires(derived_t &derived) { derived.read_impl(); };

template <typename derived_t>
concept stream_try_read_impl =
    requires(derived_t &derived) { derived.try_read_impl(); };

} // namespace detail

/// CRTP base that standardizes read/borrow/close stream APIs.
template <typename derived_t, typename value_t> class stream_base {
public:
  using value_type = value_t;
  using chunk_type = stream_chunk<value_t>;
  using chunk_view_type = stream_chunk_view<value_t>;
  using chunk_result_type = stream_result<chunk_type>;
  using chunk_try_result_type = stream_try_result<chunk_type>;
  using chunk_view_result_type = stream_result<chunk_view_type>;
  using chunk_view_try_result_type = stream_try_result<chunk_view_type>;

  /// Reads the next owned chunk using the stream's synchronous read contract.
  [[nodiscard]] auto read() -> chunk_result_type {
    clear_borrowed_cache();
    return read_owned_chunk();
  }

  /// Tries to read the next owned chunk without blocking.
  [[nodiscard]] auto try_read() -> chunk_try_result_type {
    clear_borrowed_cache();
    return try_read_owned_chunk();
  }

  /// Reads the next chunk and returns a borrowed view valid until the next
  /// read/materialization/close.
  [[nodiscard]] auto read_borrowed() -> chunk_view_result_type {
    clear_borrowed_cache();
    borrowed_read_cache_.emplace(read_owned_chunk());
    if (borrowed_read_cache_->has_error()) {
      return chunk_view_result_type::failure(borrowed_read_cache_->error());
    }
    return borrow_chunk_until_next(borrowed_read_cache_->value());
  }

  /// Tries to read the next chunk and returns a borrowed view on success.
  [[nodiscard]] auto try_read_borrowed() -> chunk_view_try_result_type {
    clear_borrowed_cache();
    auto next = try_read_owned_chunk();
    if (std::holds_alternative<stream_signal>(next)) {
      return stream_pending;
    }
    borrowed_poll_cache_.emplace(std::move(std::get<chunk_result_type>(next)));
    if (borrowed_poll_cache_->has_error()) {
      return chunk_view_result_type::failure(borrowed_poll_cache_->error());
    }
    return borrow_chunk_until_next(borrowed_poll_cache_->value());
  }

  /// Closes stream and clears borrowed cache.
  auto close() -> wh::core::result<void> {
    clear_borrowed_cache();
    return derived().close_impl();
  }

  /// Returns closure status from derived stream.
  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return derived().is_closed_impl();
  }

private:
  /// Downcasts to mutable derived type.
  [[nodiscard]] auto derived() noexcept -> derived_t & {
    return static_cast<derived_t &>(*this);
  }

  /// Downcasts to const derived type.
  [[nodiscard]] auto derived() const noexcept -> const derived_t & {
    return static_cast<const derived_t &>(*this);
  }

  auto clear_borrowed_cache() noexcept -> void {
    borrowed_read_cache_.reset();
    borrowed_poll_cache_.reset();
  }

  [[nodiscard]] auto read_owned_chunk() -> chunk_result_type {
    if constexpr (detail::stream_read_impl<derived_t>) {
      return derived().read_impl();
    } else {
      static_assert(detail::stream_always_false_v<derived_t>,
                    "stream reader must provide read_impl");
    }
  }

  [[nodiscard]] auto try_read_owned_chunk() -> chunk_try_result_type {
    if constexpr (detail::stream_try_read_impl<derived_t>) {
      return derived().try_read_impl();
    } else {
      static_assert(detail::stream_always_false_v<derived_t>,
                    "stream reader must provide try_read_impl");
    }
  }

  /// Last owned chunk cached to back borrowed-view reads.
  std::optional<chunk_result_type> borrowed_read_cache_{};
  std::optional<chunk_result_type> borrowed_poll_cache_{};
};

} // namespace wh::schema::stream
