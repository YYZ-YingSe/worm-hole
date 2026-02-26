#pragma once

#include <type_traits>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/output/callback_extra.hpp"
#include "wh/schema/stream.hpp"

namespace wh::output {

template <typename parser_t, typename input_t, typename output_t>
concept output_parser_impl = requires(parser_t &parser, const input_t &input,
                                      const callback_extra_payload &extra) {
  {
    parser.parse_value_impl(input, extra)
  } -> std::same_as<wh::core::result<output_t>>;
};

template <typename parser_t, typename input_t, typename output_t>
concept output_parser_view_impl =
    requires(parser_t &parser, const input_t &input,
             const callback_extra_view &extra) {
      {
        parser.parse_value_view_impl(input, extra)
      } -> std::same_as<wh::core::result<output_t>>;
    };

template <typename parser_t>
concept output_parser_error_observer =
    requires(parser_t &parser, const wh::core::error_code error,
             const callback_extra_payload &extra) {
      parser.on_error_impl(error, extra);
    };

template <typename parser_t>
concept output_parser_error_observer_view =
    requires(parser_t &parser, const wh::core::error_code error,
             const callback_extra_view &extra) {
      parser.on_error_view_impl(error, extra);
    };

template <typename derived_t, typename input_t, typename output_t>
class output_parser_base {
public:
  using input_type = input_t;
  using output_type = output_t;
  using input_chunk = wh::schema::stream::stream_chunk<input_t>;
  using output_chunk = wh::schema::stream::stream_chunk<output_t>;

  [[nodiscard]] auto parse_value(const input_t &input,
                                 const callback_extra_payload &extra = {})
      -> wh::core::result<output_t> {
    static_assert(
        output_parser_impl<derived_t, input_t, output_t>,
        "derived parser must implement parse_value_impl(input, extra)");
    try {
      return derived().parse_value_impl(input, extra);
    } catch (...) {
      notify_error(wh::core::make_error(wh::core::errc::parse_error), extra);
      return wh::core::result<output_t>::failure(wh::core::errc::parse_error);
    }
  }

  [[nodiscard]] auto parse_value_view(
      const input_t &input, const callback_extra_view &extra = {},
      const callback_extra_payload &fallback_extra = {})
      -> wh::core::result<output_t> {
    if constexpr (output_parser_view_impl<derived_t, input_t, output_t>) {
      try {
        return derived().parse_value_view_impl(input, extra);
      } catch (...) {
        notify_error_view(wh::core::make_error(wh::core::errc::parse_error),
                          extra, fallback_extra);
        return wh::core::result<output_t>::failure(wh::core::errc::parse_error);
      }
    } else {
      return parse_value(input, fallback_extra);
    }
  }

  [[nodiscard]] auto
  parse_stream_chunk(const input_chunk &chunk,
                     const callback_extra_payload &extra = {})
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
      return wh::core::result<output_chunk>::failure(
          wh::core::errc::parse_error);
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

  template <wh::schema::stream::stream_reader_like reader_t>
  [[nodiscard]] auto parse_stream(reader_t reader,
                                  const callback_extra_payload &extra = {})
      -> decltype(auto)
    requires std::same_as<typename reader_t::value_type, input_t>
  {
    return wh::schema::stream::convert(
        std::move(reader),
        [this, extra](const input_t &value) mutable -> wh::core::result<output_t> {
          auto parsed = parse_value(value, extra);
          if (parsed.has_error()) {
            notify_error(parsed.error(), extra);
          }
          return parsed;
        });
  }

  template <wh::schema::stream::stream_reader_like reader_t>
  [[nodiscard]] auto
  parse_stream_view(reader_t reader, const callback_extra_view &extra = {},
                    const callback_extra_payload &fallback_extra = {})
      -> decltype(auto)
    requires std::same_as<typename reader_t::value_type, input_t>
  {
    return wh::schema::stream::convert(
        std::move(reader),
        [this, extra, fallback_extra](const input_t &value) mutable
            -> wh::core::result<output_t> {
          auto parsed = parse_value_view(value, extra, fallback_extra);
          if (parsed.has_error()) {
            notify_error_view(parsed.error(), extra, fallback_extra);
          }
          return parsed;
        });
  }

private:
  auto notify_error(const wh::core::error_code error,
                    const callback_extra_payload &extra) -> void {
    if constexpr (output_parser_error_observer<derived_t>) {
      derived().on_error_impl(error, extra);
    }
  }

  auto notify_error_view(const wh::core::error_code error,
                         const callback_extra_view &extra,
                         const callback_extra_payload &fallback_extra) -> void {
    if constexpr (output_parser_error_observer_view<derived_t>) {
      derived().on_error_view_impl(error, extra);
    } else {
      notify_error(error, fallback_extra);
    }
  }

  [[nodiscard]] auto derived() noexcept -> derived_t & {
    return static_cast<derived_t &>(*this);
  }
};

} // namespace wh::output
