// Defines stream readers that transform chunk payloads.
#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/intrusive_ptr.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/schema/stream/adapter/detail/adapter_support.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::schema::stream {

template <stream_reader reader_t, typename transform_t>
class transform_stream_reader final
    : public stream_base<
          transform_stream_reader<reader_t, transform_t>,
          typename wh::core::remove_cvref_t<wh::core::callable_result_t<
              transform_t &,
              const typename reader_t::value_type &>>::value_type> {
private:
  using input_value_t = typename reader_t::value_type;
  using transform_result_t = wh::core::remove_cvref_t<
      wh::core::callable_result_t<transform_t &, const input_value_t &>>;

public:
  static_assert(wh::core::is_result_v<transform_result_t>,
                "transform callback must return wh::core::result<T>");
  static_assert(!std::same_as<typename transform_result_t::value_type, void>,
                "transform callback result value_type cannot be void");

  using value_type = typename transform_result_t::value_type;
  using input_chunk_type = stream_chunk<input_value_t>;
  using input_chunk_view_type = stream_chunk_view<input_value_t>;
  using chunk_type = stream_chunk<value_type>;

  transform_stream_reader(const transform_stream_reader &) = delete;
  auto operator=(const transform_stream_reader &)
      -> transform_stream_reader & = delete;
  transform_stream_reader(transform_stream_reader &&) noexcept = default;
  auto operator=(transform_stream_reader &&) noexcept
      -> transform_stream_reader & = default;
  ~transform_stream_reader() = default;

  template <typename source_reader_t, typename source_transform_t>
    requires std::constructible_from<reader_t, source_reader_t &&> &&
             std::constructible_from<transform_t, source_transform_t &&>
  transform_stream_reader(source_reader_t &&reader,
                          source_transform_t &&transform)
      : state_(wh::core::detail::make_intrusive<state>(
            reader_t{std::forward<source_reader_t>(reader)},
            transform_t{std::forward<source_transform_t>(transform)})) {}

  [[nodiscard]] auto read_impl() -> stream_result<chunk_type> {
    try {
      if constexpr (borrowed_stream_reader<reader_t>) {
        return map_read_borrowed(state_, state_->reader.read_borrowed());
      } else {
        return map_read_owned(state_, state_->reader.read());
      }
    } catch (...) {
      state_->adapter_state.terminal = true;
      state_->adapter_state.close_source_if_enabled(state_->reader);
      return stream_result<chunk_type>{detail::make_error_chunk<chunk_type>(
          wh::core::map_current_exception())};
    }
  }

  [[nodiscard]] auto try_read_impl() -> stream_try_result<chunk_type> {
    try {
      if constexpr (borrowed_stream_reader<reader_t>) {
        return map_try_borrowed(state_, state_->reader.try_read_borrowed());
      } else {
        return map_try_owned(state_, state_->reader.try_read());
      }
    } catch (...) {
      state_->adapter_state.terminal = true;
      state_->adapter_state.close_source_if_enabled(state_->reader);
      return stream_result<chunk_type>{detail::make_error_chunk<chunk_type>(
          wh::core::map_current_exception())};
    }
  }

  [[nodiscard]] auto read_async() const
    requires detail::async_stream_reader<reader_t>
  {
    using input_result_t = stream_result<input_chunk_type>;
    auto state = state_;
    return state->reader.read_async() |
           stdexec::then([](auto status) {
             return input_result_t{std::move(status)};
           }) |
           stdexec::upon_error([](auto &&) noexcept {
             return input_result_t::failure(wh::core::errc::internal_error);
           }) |
           stdexec::then([state = std::move(state)](input_result_t next) {
             return map_read_owned(state, std::move(next));
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
    transform_t transform;
    detail::stream_adapter_state adapter_state{};

    state(reader_t reader_value, transform_t transform_value)
        : reader(std::move(reader_value)),
          transform(std::move(transform_value)) {}
  };

  [[nodiscard]] static auto
  map_read_borrowed(const wh::core::detail::intrusive_ptr<state> &state,
                    stream_result<input_chunk_view_type> next)
      -> stream_result<chunk_type> {
    if (next.has_error()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return stream_result<chunk_type>::failure(next.error());
    }

    const auto input_chunk = next.value();
    if (input_chunk.eof) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      auto eof_chunk = chunk_type::make_eof();
      eof_chunk.source = std::string{input_chunk.source};
      return stream_result<chunk_type>{std::move(eof_chunk)};
    }
    if (input_chunk.error.failed()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      auto output_chunk = chunk_type{};
      output_chunk.source = std::string{input_chunk.source};
      output_chunk.error = input_chunk.error;
      return stream_result<chunk_type>{std::move(output_chunk)};
    }
    if (input_chunk.value == nullptr) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return stream_result<chunk_type>::failure(wh::core::errc::protocol_error);
    }

    auto mapped = map_value(state, input_chunk.source, *input_chunk.value);
    if (mapped.has_error()) {
      return stream_result<chunk_type>::failure(mapped.error());
    }
    return mapped;
  }

  [[nodiscard]] static auto
  map_read_owned(const wh::core::detail::intrusive_ptr<state> &state,
                 stream_result<input_chunk_type> next)
      -> stream_result<chunk_type> {
    if (next.has_error()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return stream_result<chunk_type>::failure(next.error());
    }

    auto input_chunk = std::move(next).value();
    if (input_chunk.eof) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      auto eof_chunk = chunk_type::make_eof();
      eof_chunk.source = std::move(input_chunk.source);
      return stream_result<chunk_type>{std::move(eof_chunk)};
    }
    if (input_chunk.error.failed()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      auto output_chunk = chunk_type{};
      output_chunk.source = std::move(input_chunk.source);
      output_chunk.error = input_chunk.error;
      return stream_result<chunk_type>{std::move(output_chunk)};
    }
    if (!input_chunk.value.has_value()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return stream_result<chunk_type>::failure(wh::core::errc::protocol_error);
    }
    return map_value(state, input_chunk.source, *input_chunk.value);
  }

  [[nodiscard]] static auto
  map_try_borrowed(const wh::core::detail::intrusive_ptr<state> &state,
                   stream_try_result<input_chunk_view_type> next)
      -> stream_try_result<chunk_type> {
    if (std::holds_alternative<stream_signal>(next)) {
      return stream_pending;
    }
    return map_read_borrowed(
        state, std::move(std::get<stream_result<input_chunk_view_type>>(next)));
  }

  [[nodiscard]] static auto
  map_try_owned(const wh::core::detail::intrusive_ptr<state> &state,
                stream_try_result<input_chunk_type> next)
      -> stream_try_result<chunk_type> {
    if (std::holds_alternative<stream_signal>(next)) {
      return stream_pending;
    }
    return map_read_owned(
        state, std::move(std::get<stream_result<input_chunk_type>>(next)));
  }

  template <typename value_u>
  [[nodiscard]] static auto
  map_value(const wh::core::detail::intrusive_ptr<state> &state,
            std::string_view source, value_u &&value)
      -> stream_result<chunk_type> {
    transform_result_t converted{};
    try {
      converted = state->transform(std::forward<value_u>(value));
    } catch (...) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return stream_result<chunk_type>{detail::make_error_chunk<chunk_type>(
          wh::core::map_current_exception())};
    }

    if (converted.has_error()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      auto output_chunk = chunk_type{};
      output_chunk.source = std::string{source};
      output_chunk.error = converted.error();
      return stream_result<chunk_type>{std::move(output_chunk)};
    }

    auto output_chunk = chunk_type::make_value(std::move(converted).value());
    output_chunk.source = std::string{source};
    return stream_result<chunk_type>{std::move(output_chunk)};
  }

  wh::core::detail::intrusive_ptr<state> state_{};
};

template <typename reader_t, typename transform_t>
  requires stream_reader<std::remove_cvref_t<reader_t>>
[[nodiscard]] inline auto make_transform_stream_reader(reader_t &&reader,
                                                       transform_t &&transform)
    -> transform_stream_reader<std::remove_cvref_t<reader_t>,
                               std::remove_cvref_t<transform_t>> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  using stored_transform_t = std::remove_cvref_t<transform_t>;
  return transform_stream_reader<stored_reader_t, stored_transform_t>{
      stored_reader_t{std::forward<reader_t>(reader)},
      stored_transform_t{std::forward<transform_t>(transform)}};
}

} // namespace wh::schema::stream
