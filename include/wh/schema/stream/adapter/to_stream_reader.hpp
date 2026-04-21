// Defines adapters that wrap plain reader values into stream chunks.
#pragma once

#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/intrusive_ptr.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/schema/stream/adapter/detail/adapter_support.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::schema::stream {

template <stream_reader reader_t>
class to_stream_reader final
    : public stream_base<to_stream_reader<reader_t>, typename reader_t::value_type> {
public:
  using value_type = typename reader_t::value_type;
  using chunk_type = stream_chunk<value_type>;

  to_stream_reader(const to_stream_reader &) = delete;
  auto operator=(const to_stream_reader &) -> to_stream_reader & = delete;
  to_stream_reader(to_stream_reader &&) noexcept = default;
  auto operator=(to_stream_reader &&) noexcept -> to_stream_reader & = default;
  ~to_stream_reader() = default;

  explicit to_stream_reader(reader_t reader)
      : state_(wh::core::detail::make_intrusive<state>(std::move(reader))) {}

  [[nodiscard]] auto read_impl() -> stream_result<chunk_type> {
    if (state_->adapter_state.terminal) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }

    stream_result<chunk_type> next{};
    try {
      next = state_->reader.read();
    } catch (...) {
      state_->adapter_state.terminal = true;
      state_->adapter_state.close_source_if_enabled(state_->reader);
      return stream_result<chunk_type>{
          detail::make_error_chunk<chunk_type>(wh::core::map_current_exception())};
    }
    return map_read_chunk(state_, std::move(next));
  }

  [[nodiscard]] auto try_read_impl() -> stream_try_result<chunk_type> {
    if (state_->adapter_state.terminal) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }

    stream_try_result<chunk_type> next{};
    try {
      next = state_->reader.try_read();
    } catch (...) {
      state_->adapter_state.terminal = true;
      state_->adapter_state.close_source_if_enabled(state_->reader);
      return stream_result<chunk_type>{
          detail::make_error_chunk<chunk_type>(wh::core::map_current_exception())};
    }
    return map_try_chunk(state_, std::move(next));
  }

  [[nodiscard]] auto read_async() const
    requires detail::async_stream_reader<reader_t>
  {
    using input_result_t = stream_result<chunk_type>;
    auto shared_state = state_;
    // Build the child sender before moving shared_state into any closure.
    // Function-argument evaluation order is not guaranteed here.
    auto sender = shared_state->reader.read_async();
    return std::move(sender) |
           stdexec::then([](auto status) { return input_result_t{std::move(status)}; }) |
           stdexec::upon_error([](auto &&) noexcept {
             return input_result_t::failure(wh::core::errc::internal_error);
           }) |
           stdexec::then([shared_state = std::move(shared_state)](input_result_t next) {
             return map_read_chunk(shared_state, std::move(next));
           });
  }

  auto close_impl() -> wh::core::result<void> {
    state_->adapter_state.terminal = true;
    return state_->adapter_state.close_source(state_->reader);
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return state_->adapter_state.terminal || state_->reader.is_closed();
  }

  auto set_automatic_close(const auto_close_options &options) -> void {
    state_->adapter_state.automatic_close = options.enabled;
    detail::set_automatic_close_if_supported(state_->reader, options);
  }

private:
  struct state : wh::core::detail::intrusive_enable_from_this<state> {
    reader_t reader;
    detail::stream_adapter_state adapter_state{};

    explicit state(reader_t reader_value) : reader(std::move(reader_value)) {}
  };

  [[nodiscard]] static auto map_read_chunk(const wh::core::detail::intrusive_ptr<state> &state,
                                           stream_result<chunk_type> next)
      -> stream_result<chunk_type> {
    if (next.has_error()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return stream_result<chunk_type>::failure(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.is_terminal_eof() || chunk.error.failed()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
    }
    return stream_result<chunk_type>{std::move(chunk)};
  }

  [[nodiscard]] static auto map_try_chunk(const wh::core::detail::intrusive_ptr<state> &state,
                                          stream_try_result<chunk_type> next)
      -> stream_try_result<chunk_type> {
    if (std::holds_alternative<stream_signal>(next)) {
      return stream_pending;
    }
    return map_read_chunk(state, std::move(std::get<stream_result<chunk_type>>(next)));
  }

  wh::core::detail::intrusive_ptr<state> state_{};
};

template <typename reader_t>
  requires stream_reader<std::remove_cvref_t<reader_t>>
[[nodiscard]] inline auto make_to_stream_reader(reader_t &&reader)
    -> to_stream_reader<std::remove_cvref_t<reader_t>> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  return to_stream_reader<stored_reader_t>{stored_reader_t{std::forward<reader_t>(reader)}};
}

} // namespace wh::schema::stream
