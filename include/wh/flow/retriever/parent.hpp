// Defines parent-document recovery flow on top of a child-document retriever.
#pragma once

#include <algorithm>
#include <concepts>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/document.hpp"

namespace wh::flow::retriever {

inline constexpr std::string_view parent_id_metadata_key = "parent_id";

namespace parent_detail {

template <typename retriever_t>
concept retriever_component =
    requires(const retriever_t &retriever,
             const wh::retriever::retriever_request &request,
             wh::core::run_context &context) {
      { retriever.retrieve(request, context) }
      -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
    };

template <typename loader_t>
concept parent_loader =
    requires(loader_t loader, const std::vector<std::string> &parent_ids,
             wh::core::run_context &context) {
      { loader(parent_ids, context) }
      -> std::same_as<wh::core::result<std::vector<wh::schema::document>>>;
    };

} // namespace parent_detail

/// Parent retriever that maps child hits back to unique parent documents.
template <parent_detail::retriever_component retriever_t,
          parent_detail::parent_loader parent_loader_t>
class parent {
public:
  parent(retriever_t child_retriever, parent_loader_t parent_loader) noexcept
      : child_retriever_(std::move(child_retriever)),
        parent_loader_(std::move(parent_loader)) {}

  /// Runs child retrieval, extracts stable parent ids, and loads parent docs in
  /// first-hit order.
  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request,
                              wh::core::run_context &context) const
      -> wh::core::result<wh::retriever::retriever_response> {
    auto child_hits = child_retriever_.retrieve(request, context);
    if (child_hits.has_error()) {
      return wh::core::result<wh::retriever::retriever_response>::failure(
          child_hits.error());
    }

    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        seen{};
    std::vector<std::string> parent_ids{};
    for (const auto &document : child_hits.value()) {
      const auto *parent_id =
          document.template metadata_ptr<std::string>(parent_id_metadata_key);
      if (parent_id == nullptr || parent_id->empty()) {
        continue;
      }
      if (seen.insert(*parent_id).second) {
        parent_ids.push_back(*parent_id);
      }
    }

    auto loaded = parent_loader_(parent_ids, context);
    if (loaded.has_error()) {
      return wh::core::result<wh::retriever::retriever_response>::failure(
          loaded.error());
    }

    std::unordered_map<std::string, wh::schema::document,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        documents{};
    for (auto &document : loaded.value()) {
      const auto *parent_id =
          document.template metadata_ptr<std::string>(parent_id_metadata_key);
      if (parent_id == nullptr) {
        continue;
      }
      documents.insert_or_assign(*parent_id, std::move(document));
    }

    wh::retriever::retriever_response ordered{};
    for (const auto &parent_id : parent_ids) {
      const auto iter = documents.find(parent_id);
      if (iter != documents.end()) {
        ordered.push_back(iter->second);
      }
    }
    return ordered;
  }

private:
  retriever_t child_retriever_;
  parent_loader_t parent_loader_;
};

} // namespace wh::flow::retriever
