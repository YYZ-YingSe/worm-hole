// Defines the built-in document processing implementation composed from
// loader, transformer, and parser stages.
#pragma once

#include <concepts>
#include <string>
#include <type_traits>
#include <utility>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/component.hpp"
#include "wh/core/error.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/document/callback_event_loader.hpp"
#include "wh/document/callback_event_parser.hpp"
#include "wh/document/callback_event_transformer.hpp"
#include "wh/document/document.hpp"
#include "wh/document/parser/ext_parser.hpp"

namespace wh::document {

using loader_function =
    wh::core::function<wh::core::result<std::string>(std::string, const loader_options &) const>;
using transformer_function = wh::core::function<wh::core::result<std::string>(std::string) const>;

namespace detail {

using callback_sink = wh::callbacks::callback_sink;
using wh::callbacks::borrow_callback_sink;
using wh::callbacks::make_callback_sink;

template <typename payload_t>
inline auto emit_callback(const callback_sink &sink, const wh::callbacks::stage stage,
                          const payload_t &payload, const loader_options &options) -> void {
  wh::callbacks::run_info run_info{};
  run_info.name = "DocumentProcessor";
  run_info.type = "DocumentProcessor";
  run_info.component = wh::core::component_kind::document;
  run_info = wh::callbacks::apply_component_run_info(std::move(run_info), options);
  wh::callbacks::emit(sink, stage, payload, run_info);
}

} // namespace detail

class document_processor {
public:
  document_processor() : parser_(parser::ext_parser{}) {}
  explicit document_processor(const parser::parser &parser) : parser_(parser) {}
  explicit document_processor(parser::parser &&parser) : parser_(std::move(parser)) {}

  template <typename parser_t>
    requires parser::parser_like<std::remove_cvref_t<parser_t>> &&
             (!std::same_as<std::remove_cvref_t<parser_t>, parser::parser>)
  explicit document_processor(parser_t &&parser_impl)
      : parser_(parser::parser{std::forward<parser_t>(parser_impl)}) {}

  document_processor(const document_processor &other) : parser_(other.parser_) {
    if (other.loader_ != nullptr) {
      loader_ = other.loader_;
    }
    if (other.transformer_ != nullptr) {
      transformer_ = other.transformer_;
    }
  }
  document_processor(document_processor &&) noexcept = default;

  auto operator=(const document_processor &other) -> document_processor & {
    if (this == &other) {
      return *this;
    }
    parser_ = other.parser_;
    loader_ = nullptr;
    transformer_ = nullptr;
    if (other.loader_ != nullptr) {
      loader_ = other.loader_;
    }
    if (other.transformer_ != nullptr) {
      transformer_ = other.transformer_;
    }
    return *this;
  }

  auto operator=(document_processor &&) noexcept -> document_processor & = default;
  ~document_processor() = default;

  [[nodiscard]] static auto descriptor() -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"DocumentProcessor", wh::core::component_kind::document};
  }

  template <typename loader_t>
    requires std::constructible_from<loader_function, loader_t &&>
  auto set_loader(loader_t &&loader) -> document_processor & {
    loader_ = loader_function{std::forward<loader_t>(loader)};
    return *this;
  }

  auto set_loader(std::nullptr_t) -> document_processor & {
    loader_ = nullptr;
    return *this;
  }

  template <typename transformer_t>
    requires std::constructible_from<transformer_function, transformer_t &&>
  auto set_transformer(transformer_t &&transformer) -> document_processor & {
    transformer_ = transformer_function{std::forward<transformer_t>(transformer)};
    return *this;
  }

  auto set_transformer(std::nullptr_t) -> document_processor & {
    transformer_ = nullptr;
    return *this;
  }

  auto set_parser(const parser::parser &parser) -> document_processor & {
    parser_ = parser;
    return *this;
  }

  auto set_parser(parser::parser &&parser) -> document_processor & {
    parser_ = std::move(parser);
    return *this;
  }

  template <typename parser_t>
    requires parser::parser_like<std::remove_cvref_t<parser_t>>
  auto set_parser(parser_t &&parser_impl) -> document_processor & {
    parser_ = parser::parser{std::forward<parser_t>(parser_impl)};
    return *this;
  }

  [[nodiscard]] auto process(const document_request &request,
                             wh::core::run_context &callback_context) const
      -> wh::core::result<document_batch> {
    return process_impl(request, detail::borrow_callback_sink(callback_context));
  }

  [[nodiscard]] auto process(document_request &&request,
                             wh::core::run_context &callback_context) const
      -> wh::core::result<document_batch> {
    return process_impl(std::move(request), detail::borrow_callback_sink(callback_context));
  }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, document_request>
  [[nodiscard]] auto process_impl(request_t &&request, detail::callback_sink sink) const
      -> wh::core::result<document_batch> {
    document_request stored{std::forward<request_t>(request)};
    sink = wh::callbacks::filter_callback_sink(std::move(sink), stored.options);
    std::string source_uri = stored.source;
    detail::emit_callback(sink, wh::callbacks::stage::start,
                          parser_callback_event{source_uri, 0U, 0U}, stored.options);

    if (!parser_.has_value()) {
      detail::emit_callback(sink, wh::callbacks::stage::error,
                            parser_callback_event{source_uri, 0U, 0U}, stored.options);
      return wh::core::result<document_batch>::failure(wh::core::errc::not_supported);
    }

    const auto resolved_options = stored.options.resolve_view();
    std::string content{};
    if (stored.source_kind == document_source_kind::uri && loader_) {
      auto loaded = loader_(source_uri, stored.options);
      if (loaded.has_error()) {
        detail::emit_callback(sink, wh::callbacks::stage::error,
                              parser_callback_event{source_uri, 0U, 0U}, stored.options);
        return wh::core::result<document_batch>::failure(loaded.error());
      }
      content = std::move(loaded).value();
      detail::emit_callback(sink, wh::callbacks::stage::end,
                            loader_callback_event{source_uri, content.size()}, stored.options);
    } else {
      content = source_uri;
    }

    if (transformer_) {
      auto transformed = transformer_(content);
      if (transformed.has_error()) {
        detail::emit_callback(sink, wh::callbacks::stage::error,
                              parser_callback_event{source_uri, 0U, 0U}, stored.options);
        return wh::core::result<document_batch>::failure(transformed.error());
      }
      content = std::move(transformed).value();
      detail::emit_callback(sink, wh::callbacks::stage::end, transformer_callback_event{1U, 1U},
                            stored.options);
    }

    const std::string_view parse_source_uri = resolved_options.parser_uri().empty()
                                                  ? std::string_view{source_uri}
                                                  : resolved_options.parser_uri();
    parser::parse_options_view parse_options_view{};
    parse_options_view.uri = parse_source_uri;
    parse_options_view.extra_meta_base = &resolved_options.base_parser->extra_meta;
    parse_options_view.extra_meta_override = resolved_options.override_parser == nullptr
                                                 ? nullptr
                                                 : &resolved_options.override_parser->extra_meta;
    parse_options_view.format_options_base = &resolved_options.base_parser->format_options;
    parse_options_view.format_options_override =
        resolved_options.override_parser == nullptr
            ? nullptr
            : &resolved_options.override_parser->format_options;

    parser::parse_request_view parse_request{};
    parse_request.content = content;
    parse_request.options = parse_options_view;
    const auto parser_input_bytes = parse_request.content.size();
    auto parsed = parser_.parse(parse_request);
    if (parsed.has_error()) {
      detail::emit_callback(
          sink, wh::callbacks::stage::error,
          parser_callback_event{std::string{parse_source_uri}, parser_input_bytes, 0U},
          stored.options);
      return parsed;
    }

    auto documents = std::move(parsed).value();
    for (auto &doc : documents) {
      if (doc.content().empty()) {
        continue;
      }
      doc.set_metadata("_source", std::string{parse_source_uri});
      parse_request.options.for_each_extra_meta(
          [&](const std::string &key, const std::string &value) { doc.set_metadata(key, value); });
    }

    detail::emit_callback(
        sink, wh::callbacks::stage::end,
        parser_callback_event{std::string{parse_source_uri}, parser_input_bytes, documents.size()},
        stored.options);
    return documents;
  }

  loader_function loader_{nullptr};
  transformer_function transformer_{nullptr};
  parser::parser parser_{parser::ext_parser{}};
};

} // namespace wh::document
