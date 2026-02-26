#pragma once

#include <optional>

#include "wh/core/result.hpp"
#include "wh/schema/stream/types.hpp"

namespace wh::schema::stream {

template <typename derived_t, typename value_t> class stream_base {
public:
  using value_type = value_t;
  using chunk_type = stream_chunk<value_t>;
  using chunk_view_type = stream_chunk_view<value_t>;

  [[nodiscard]] auto try_read() -> wh::core::result<chunk_type> {
    borrowed_chunk_cache_.reset();
    return derived().next_impl();
  }

  [[nodiscard]] auto try_read_borrowed_until_next()
      -> wh::core::result<chunk_view_type> {
    borrowed_chunk_cache_.emplace(derived().next_impl());
    if (borrowed_chunk_cache_->has_error()) {
      return wh::core::result<chunk_view_type>::failure(
          borrowed_chunk_cache_->error());
    }
    return borrow_chunk_until_next(borrowed_chunk_cache_->value());
  }

  [[nodiscard]] auto next() -> wh::core::result<chunk_type> {
    return try_read();
  }

  auto close() -> wh::core::result<void> {
    borrowed_chunk_cache_.reset();
    return derived().close_impl();
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return derived().is_closed_impl();
  }

private:
  [[nodiscard]] auto derived() noexcept -> derived_t & {
    return static_cast<derived_t &>(*this);
  }

  [[nodiscard]] auto derived() const noexcept -> const derived_t & {
    return static_cast<const derived_t &>(*this);
  }

  std::optional<wh::core::result<chunk_type>> borrowed_chunk_cache_{};
};

} // namespace wh::schema::stream
