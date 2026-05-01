// Defines output parser contracts for converting model text/stream output
// into typed structured results with callback support.
#pragma once

#include <type_traits>
#include <utility>

#include "wh/callbacks/interface.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/stream/adapter/transform_stream_reader.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace wh::output {

template <typename parser_t, typename input_t, typename output_t>
concept output_parser_impl =
    requires(parser_t &parser, const input_t &input, const wh::callbacks::event_payload &extra) {
      { parser.parse_value_impl(input, extra) } -> std::same_as<wh::core::result<output_t>>;
    };

template <typename parser_t, typename input_t, typename output_t>
concept output_parser_view_impl =
    requires(parser_t &parser, const input_t &input, const wh::callbacks::event_view &extra) {
      { parser.parse_value_view_impl(input, extra) } -> std::same_as<wh::core::result<output_t>>;
    };

template <typename parser_t>
concept output_parser_error_observer =
    requires(parser_t &parser, const wh::core::error_code error,
             const wh::callbacks::event_payload &extra) { parser.on_error_impl(error, extra); };

template <typename parser_t>
concept output_parser_error_observer_view =
    requires(parser_t &parser, const wh::core::error_code error,
             const wh::callbacks::event_view &extra) { parser.on_error_view_impl(error, extra); };

/// Public interface for `output_parser_base`.
template <typename derived_t, typename input_t, typename output_t> class output_parser_base {
public:
  using input_type = input_t;
  using output_type = output_t;
  using input_chunk = wh::schema::stream::stream_chunk<input_t>;
  using output_chunk = wh::schema::stream::stream_chunk<output_t>;

  /// Parses a fully materialized input value and emits parser callbacks with
  /// typed result output.
  [[nodiscard]] auto parse_value(const input_t &input,
                                 const wh::callbacks::event_payload &extra = {})
      -> wh::core::result<output_t> {
    static_assert(output_parser_impl<derived_t, input_t, output_t>,
                  "derived parser must implement parse_value_impl(input, extra)");
    try {
      return derived().parse_value_impl(input, extra);
    } catch (...) {
      notify_error(wh::core::make_error(wh::core::errc::parse_error), extra);
      return wh::core::result<output_t>::failure(wh::core::errc::parse_error);
    }
  }

  /// Parses an input view and emits parser callbacks with typed result output.
  [[nodiscard]] auto parse_value_view(const input_t &input,
                                      const wh::callbacks::event_view &extra = {},
                                      const wh::callbacks::event_payload &fallback_extra = {})
      -> wh::core::result<output_t> {
    if constexpr (output_parser_view_impl<derived_t, input_t, output_t>) {
      try {
        return derived().parse_value_view_impl(input, extra);
      } catch (...) {
        notify_error_view(wh::core::make_error(wh::core::errc::parse_error), extra, fallback_extra);
        return wh::core::result<output_t>::failure(wh::core::errc::parse_error);
      }
    } else {
      return parse_value(input, fallback_extra);
    }
  }

  /// Parses one stream chunk and optionally emits a parsed output item.
  [[nodiscard]] auto parse_stream_chunk(const input_chunk &chunk,
                                        const wh::callbacks::event_payload &extra = {})
      -> wh::core::result<output_chunk> {
    if (chunk.error.failed()) {
      notify_error(chunk.error, extra);
      return wh::core::result<output_chunk>::failure(chunk.error.code());
    }
    if (chunk.eof) {
      output_chunk output = output_chunk::make_eof();
      output.source = chunk.source;
      return output;
    }
    if (!chunk.value.has_value()) {
      notify_error(wh::core::make_error(wh::core::errc::parse_error), extra);
      return wh::core::result<output_chunk>::failure(wh::core::errc::parse_error);
    }

    auto parsed = parse_value(*chunk.value, extra);
    if (parsed.has_error()) {
      notify_error(parsed.error(), extra);
      return wh::core::result<output_chunk>::failure(parsed.error());
    }

    output_chunk output = output_chunk::make_value(std::move(parsed).value());
    output.source = chunk.source;
    return output;
  }

  template <wh::schema::stream::stream_reader reader_t>
  /// Parses a stream reader to completion and returns all parsed items.
  [[nodiscard]] auto parse_stream(reader_t &&reader, const wh::callbacks::event_payload &extra = {})
      -> decltype(auto)
    requires std::same_as<typename std::remove_cvref_t<reader_t>::value_type, input_t>
  {
    return wh::schema::stream::make_transform_stream_reader(
        std::forward<reader_t>(reader),
        [this, extra](const input_t &value) mutable -> wh::core::result<output_t> {
          auto parsed = parse_value(value, extra);
          if (parsed.has_error()) {
            notify_error(parsed.error(), extra);
          }
          return parsed;
        });
  }

  template <wh::schema::stream::stream_reader reader_t>
  /// Parses a stream view to completion and returns all parsed items.
  [[nodiscard]] auto parse_stream_view(reader_t &&reader,
                                       const wh::callbacks::event_view &extra = {},
                                       const wh::callbacks::event_payload &fallback_extra = {})
      -> decltype(auto)
    requires std::same_as<typename std::remove_cvref_t<reader_t>::value_type, input_t>
  {
    return wh::schema::stream::make_transform_stream_reader(
        std::forward<reader_t>(reader),
        [this, extra, fallback_extra](const input_t &value) mutable -> wh::core::result<output_t> {
          auto parsed = parse_value_view(value, extra, fallback_extra);
          if (parsed.has_error()) {
            notify_error_view(parsed.error(), extra, fallback_extra);
          }
          return parsed;
        });
  }

private:
  /// Emits parser error callback with owning payload metadata.
  auto notify_error(const wh::core::error_code error, const wh::callbacks::event_payload &extra)
      -> void {
    if constexpr (output_parser_error_observer<derived_t>) {
      derived().on_error_impl(error, extra);
    }
  }

  /// Emits parser error callback with non-owning payload metadata view.
  auto notify_error_view(const wh::core::error_code error, const wh::callbacks::event_view &extra,
                         const wh::callbacks::event_payload &fallback_extra) -> void {
    if constexpr (output_parser_error_observer_view<derived_t>) {
      derived().on_error_view_impl(error, extra);
    } else {
      notify_error(error, fallback_extra);
    }
  }

  /// Returns `derived_t` reference for CRTP dispatch.
  [[nodiscard]] auto derived() noexcept -> derived_t & { return static_cast<derived_t &>(*this); }
};

} // namespace wh::output
