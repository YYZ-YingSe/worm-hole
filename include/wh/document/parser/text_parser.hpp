// Implements a plain-text document parser that converts raw text content
// into normalized document batches.
#pragma once

#include <string>

#include "wh/document/parser/interface.hpp"

namespace wh::document::parser {

class text_parser {
public:
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"TextParser",
                                          wh::core::component_kind::document};
  }

  [[nodiscard]] auto parse(const parse_request &request) const
      -> wh::core::result<document_batch> {
    wh::schema::document doc{request.content};
    doc.set_metadata("_source", request.options.uri);
    for (const auto &[key, value] : request.options.extra_meta) {
      doc.set_metadata(key, value);
    }
    return document_batch{std::move(doc)};
  }

  [[nodiscard]] auto parse(parse_request &&request) const
      -> wh::core::result<document_batch> {
    wh::schema::document doc{std::move(request.content)};
    doc.set_metadata("_source", std::move(request.options.uri));
    for (auto &[key, value] : request.options.extra_meta) {
      doc.set_metadata(key, std::move(value));
    }
    return document_batch{std::move(doc)};
  }

  [[nodiscard]] auto parse(const parse_request_view request) const
      -> wh::core::result<document_batch> {
    wh::schema::document doc{std::string{request.content}};
    doc.set_metadata("_source", std::string{request.options.uri});
    request.options.for_each_extra_meta(
        [&](const std::string &key, const std::string &value) {
          doc.set_metadata(key, value);
        });
    return document_batch{std::move(doc)};
  }
};

[[nodiscard]] inline auto make_text_parser() -> parser {
  return parser{text_parser{}};
}

} // namespace wh::document::parser
