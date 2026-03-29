// Defines parent-document indexer flow that expands parent docs into sub docs
// before delegating to the underlying indexer component.
#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/schema/document.hpp"

namespace wh::flow::indexer {

inline constexpr std::string_view parent_id_metadata_key = "parent_id";
inline constexpr std::string_view sub_id_metadata_key = "sub_id";

namespace detail {

template <typename indexer_t>
concept indexer_component =
    requires(const indexer_t &indexer, const wh::indexer::indexer_request &request,
             wh::core::run_context &context) {
      { indexer.write(request, context) }
      -> std::same_as<wh::core::result<wh::indexer::indexer_response>>;
    };

template <typename transformer_t>
concept parent_transformer =
    requires(transformer_t transformer, const wh::schema::document &document,
             wh::core::run_context &context) {
      { transformer(document, context) }
      -> std::same_as<wh::core::result<std::vector<wh::schema::document>>>;
    };

template <typename generator_t>
concept sub_id_generator =
    requires(generator_t generator, const wh::schema::document &document,
             std::size_t count, wh::core::run_context &context) {
      { generator(document, count, context) }
      -> std::same_as<wh::core::result<std::vector<std::string>>>;
    };

[[nodiscard]] inline auto effective_parent_id(const wh::schema::document &document)
    -> std::string {
  if (const auto *typed = document.metadata_ptr<std::string>(parent_id_metadata_key);
      typed != nullptr && !typed->empty()) {
    return *typed;
  }
  return document.content();
}

} // namespace detail

/// Parent indexer flow that transforms parent documents into sub documents.
template <detail::indexer_component indexer_t,
          detail::parent_transformer transformer_t,
          detail::sub_id_generator sub_id_generator_t>
class parent {
public:
  parent(indexer_t indexer, transformer_t transformer,
         sub_id_generator_t sub_id_generator) noexcept
      : indexer_(std::move(indexer)), transformer_(std::move(transformer)),
        sub_id_generator_(std::move(sub_id_generator)) {}

  /// Expands parents into grouped sub documents and delegates one batch write.
  [[nodiscard]] auto write(const wh::indexer::indexer_request &request,
                           wh::core::run_context &context) const
      -> wh::core::result<wh::indexer::indexer_response> {
    std::vector<wh::schema::document> sub_documents{};
    for (const auto &parent_document : request.documents) {
      auto transformed = transformer_(parent_document, context);
      if (transformed.has_error()) {
        return wh::core::result<wh::indexer::indexer_response>::failure(
            transformed.error());
      }
      if (transformed.value().empty()) {
        return wh::core::result<wh::indexer::indexer_response>::failure(
            wh::core::errc::invalid_argument);
      }

      const auto parent_id = detail::effective_parent_id(parent_document);
      auto sub_ids =
          sub_id_generator_(parent_document, transformed.value().size(), context);
      if (sub_ids.has_error()) {
        return wh::core::result<wh::indexer::indexer_response>::failure(
            sub_ids.error());
      }
      if (sub_ids.value().size() != transformed.value().size()) {
        return wh::core::result<wh::indexer::indexer_response>::failure(
            wh::core::errc::invalid_argument);
      }

      for (std::size_t index = 0U; index < transformed.value().size(); ++index) {
        auto document = std::move(transformed.value()[index]);
        document.set_metadata(parent_id_metadata_key, parent_id);
        document.set_metadata(sub_id_metadata_key, sub_ids.value()[index]);
        sub_documents.push_back(std::move(document));
      }
    }

    auto delegated = request;
    delegated.documents = std::move(sub_documents);
    return indexer_.write(delegated, context);
  }

private:
  indexer_t indexer_;
  transformer_t transformer_;
  sub_id_generator_t sub_id_generator_;
};

} // namespace wh::flow::indexer
