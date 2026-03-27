// Defines stream readers that can map one chunk to one output or skip it.
#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/schema/stream/adapter/detail/adapter_support.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::schema::stream {

/// Explicit marker meaning one consumed input chunk produces no output chunk.
struct skip_t {};

/// Shared skip marker instance for filter-map callbacks.
inline constexpr skip_t skip{};

template <typename value_t>
using filter_map_step = std::variant<value_t, skip_t>;

namespace detail {

template <typename step_t> struct filter_map_step_traits {
  static constexpr bool valid = false;
  using value_type = void;
};

template <typename value_t>
struct filter_map_step_traits<std::variant<value_t, skip_t>> {
  static constexpr bool valid = true;
  using value_type = value_t;
};

template <typename value_t>
struct filter_map_step_traits<std::variant<skip_t, value_t>> {
  static constexpr bool valid = true;
  using value_type = value_t;
};

template <typename result_t> struct filter_map_result_traits {
  static constexpr bool valid = false;
  using value_type = void;
  using step_type = void;
};

template <typename step_t, typename error_t>
struct filter_map_result_traits<wh::core::result<step_t, error_t>> {
  using stored_step_t = wh::core::remove_cvref_t<step_t>;
  using step_traits = filter_map_step_traits<stored_step_t>;

  static constexpr bool valid = step_traits::valid;
  using value_type = typename step_traits::value_type;
  using step_type = stored_step_t;
};

} // namespace detail

template <borrowed_stream_reader reader_t, typename filter_map_t>
class filter_map_stream_reader final
    : public stream_base<
          filter_map_stream_reader<reader_t, filter_map_t>,
          typename detail::filter_map_result_traits<
              wh::core::remove_cvref_t<wh::core::callable_result_t<
                  filter_map_t &, const typename reader_t::value_type &>>>::
              value_type> {
private:
  using input_value_t = typename reader_t::value_type;
  using filter_map_result_t = wh::core::remove_cvref_t<
      wh::core::callable_result_t<filter_map_t &, const input_value_t &>>;
  using traits_t = detail::filter_map_result_traits<filter_map_result_t>;

public:
  static_assert(wh::core::is_result_v<filter_map_result_t>,
                "filter_map callback must return wh::core::result<filter_map_step<T>>");
  static_assert(traits_t::valid,
                "filter_map callback must return wh::core::result<filter_map_step<T>>");

  using value_type = typename traits_t::value_type;
  using step_type = typename traits_t::step_type;
  using input_chunk_type = stream_chunk<input_value_t>;
  using input_chunk_view_type = stream_chunk_view<input_value_t>;
  using chunk_type = stream_chunk<value_type>;
  using async_result_sender = typename exec::any_receiver_ref<
      stdexec::completion_signatures<stdexec::set_value_t(
          stream_result<chunk_type>),
                                     stdexec::set_stopped_t()>>::template any_sender<>;

  template <typename source_reader_t, typename source_filter_map_t>
    requires std::constructible_from<reader_t, source_reader_t &&> &&
             std::constructible_from<filter_map_t, source_filter_map_t &&>
  filter_map_stream_reader(source_reader_t &&reader,
                           source_filter_map_t &&filter_map)
      : reader_(std::forward<source_reader_t>(reader)),
        filter_map_(std::forward<source_filter_map_t>(filter_map)) {}

  [[nodiscard]] auto read_impl() -> stream_result<chunk_type> {
    while (true) {
      try {
        auto mapped = map_read_borrowed(reader_.read_borrowed());
        if (std::holds_alternative<skip_t>(mapped)) {
          continue;
        }
        return std::get<stream_result<chunk_type>>(std::move(mapped));
      } catch (...) {
        state_.terminal = true;
        state_.close_source_if_enabled(reader_);
        return stream_result<chunk_type>{
            detail::make_error_chunk<chunk_type>(
                wh::core::map_current_exception())};
      }
    }
  }

  [[nodiscard]] auto try_read_impl() -> stream_try_result<chunk_type> {
    while (true) {
      try {
        auto next = reader_.try_read_borrowed();
        if (std::holds_alternative<stream_signal>(next)) {
          return stream_pending;
        }
        auto mapped = map_read_borrowed(
            std::move(std::get<stream_result<input_chunk_view_type>>(next)));
        if (std::holds_alternative<skip_t>(mapped)) {
          continue;
        }
        return std::get<stream_result<chunk_type>>(std::move(mapped));
      } catch (...) {
        state_.terminal = true;
        state_.close_source_if_enabled(reader_);
        return stream_result<chunk_type>{
            detail::make_error_chunk<chunk_type>(
                wh::core::map_current_exception())};
      }
    }
  }

  [[nodiscard]] auto read_async() const -> async_result_sender
    requires detail::async_stream_reader<reader_t>
  {
    using input_result_t = stream_result<input_chunk_type>;
    return async_result_sender{
        reader_.read_async() |
        stdexec::then([](auto status) {
          return input_result_t{std::move(status)};
        }) |
        stdexec::upon_error([](auto &&) noexcept {
          return input_result_t::failure(wh::core::errc::internal_error);
        }) |
        stdexec::let_value([this](input_result_t next) -> async_result_sender {
          auto mapped = map_read_owned(std::move(next));
          if (std::holds_alternative<skip_t>(mapped)) {
            return this->read_async();
          }
          return async_result_sender{stdexec::just(
              std::get<stream_result<chunk_type>>(std::move(mapped)))};
        })};
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
  using mapped_read_t = std::variant<skip_t, stream_result<chunk_type>>;

  [[nodiscard]] auto map_read_borrowed(
      stream_result<input_chunk_view_type> next) const -> mapped_read_t {
    if (next.has_error()) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      return stream_result<chunk_type>::failure(next.error());
    }

    const auto input_chunk = next.value();
    if (input_chunk.eof) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      auto eof_chunk = chunk_type::make_eof();
      eof_chunk.source = std::string{input_chunk.source};
      return stream_result<chunk_type>{std::move(eof_chunk)};
    }
    if (input_chunk.error.failed()) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      auto output_chunk = chunk_type{};
      output_chunk.source = std::string{input_chunk.source};
      output_chunk.error = input_chunk.error;
      return stream_result<chunk_type>{std::move(output_chunk)};
    }
    if (input_chunk.value == nullptr) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      return stream_result<chunk_type>::failure(wh::core::errc::protocol_error);
    }
    return map_value(input_chunk.source, *input_chunk.value);
  }

  [[nodiscard]] auto map_read_owned(stream_result<input_chunk_type> next) const
      -> mapped_read_t {
    if (next.has_error()) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      return stream_result<chunk_type>::failure(next.error());
    }

    auto input_chunk = std::move(next).value();
    if (input_chunk.eof) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      auto eof_chunk = chunk_type::make_eof();
      eof_chunk.source = std::move(input_chunk.source);
      return stream_result<chunk_type>{std::move(eof_chunk)};
    }
    if (input_chunk.error.failed()) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      auto output_chunk = chunk_type{};
      output_chunk.source = std::move(input_chunk.source);
      output_chunk.error = input_chunk.error;
      return stream_result<chunk_type>{std::move(output_chunk)};
    }
    if (!input_chunk.value.has_value()) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      return stream_result<chunk_type>::failure(wh::core::errc::protocol_error);
    }
    return map_value(input_chunk.source, *input_chunk.value);
  }

  template <typename value_u>
  [[nodiscard]] auto map_value(std::string_view source, value_u &&value) const
      -> mapped_read_t {
    filter_map_result_t converted{};
    try {
      converted = filter_map_(std::forward<value_u>(value));
    } catch (...) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      return stream_result<chunk_type>{
          detail::make_error_chunk<chunk_type>(
              wh::core::map_current_exception())};
    }

    if (converted.has_error()) {
      state_.terminal = true;
      state_.close_source_if_enabled(reader_);
      auto output_chunk = chunk_type{};
      output_chunk.source = std::string{source};
      output_chunk.error = converted.error();
      return stream_result<chunk_type>{std::move(output_chunk)};
    }

    auto step = std::move(converted).value();
    if (std::holds_alternative<skip_t>(step)) {
      return skip_t{};
    }

    auto output_chunk =
        chunk_type::make_value(std::get<value_type>(std::move(step)));
    output_chunk.source = std::string{source};
    return stream_result<chunk_type>{std::move(output_chunk)};
  }

  mutable reader_t reader_;
  mutable filter_map_t filter_map_;
  mutable detail::stream_adapter_state state_{};
};

template <typename reader_t, typename filter_map_t>
  requires borrowed_stream_reader<std::remove_cvref_t<reader_t>>
[[nodiscard]] inline auto make_filter_map_stream_reader(
    reader_t &&reader, filter_map_t &&filter_map)
    -> filter_map_stream_reader<std::remove_cvref_t<reader_t>,
                                std::remove_cvref_t<filter_map_t>> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  using stored_filter_map_t = std::remove_cvref_t<filter_map_t>;
  return filter_map_stream_reader<stored_reader_t, stored_filter_map_t>{
      stored_reader_t{std::forward<reader_t>(reader)},
      stored_filter_map_t{std::forward<filter_map_t>(filter_map)}};
}

} // namespace wh::schema::stream
