// Defines retriever routing and fusion flows that reuse the existing retriever
// component contract.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
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

/// One route-local retrieval result.
struct routed_retriever_result {
  /// Stable retriever name.
  std::string retriever_name{};
  /// Retrieved documents for this route.
  wh::retriever::retriever_response documents{};
};

namespace router_detail {

template <typename retriever_t>
concept retriever_component =
    requires(const retriever_t &retriever,
             const wh::retriever::retriever_request &request,
             wh::core::run_context &context) {
      { retriever.retrieve(request, context) }
      -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
    };

template <typename route_t>
concept route_policy =
    requires(route_t route, const wh::retriever::retriever_request &request,
             const std::vector<std::string> &names) {
      { route(request, names) }
      -> std::same_as<wh::core::result<std::vector<std::string>>>;
    };

template <typename fusion_t>
concept fusion_policy = requires(fusion_t fusion,
                                 const std::vector<routed_retriever_result> &results) {
  { fusion(results) } -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
};

struct route_all_retrievers {
  [[nodiscard]] auto operator()(const wh::retriever::retriever_request &,
                                const std::vector<std::string> &names) const
      -> wh::core::result<std::vector<std::string>> {
    return names;
  }
};

struct reciprocal_rank_fusion {
  [[nodiscard]] auto operator()(const std::vector<routed_retriever_result> &results) const
      -> wh::core::result<wh::retriever::retriever_response> {
    std::unordered_map<std::string, std::pair<double, std::size_t>,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        scores{};
    std::unordered_map<std::string, wh::schema::document,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        documents{};

    std::size_t sequence = 0U;
    for (const auto &result : results) {
      for (std::size_t rank = 0U; rank < result.documents.size(); ++rank) {
        const auto &document = result.documents[rank];
        const auto key = document.content();
        auto [score_iter, inserted] =
            scores.try_emplace(key, 0.0, sequence++);
        score_iter->second.first += 1.0 / (static_cast<double>(rank) + 60.0);
        if (inserted) {
          documents.emplace(key, document);
        }
      }
    }

    std::vector<std::pair<std::string, std::pair<double, std::size_t>>> ranked{};
    ranked.reserve(scores.size());
    for (const auto &entry : scores) {
      ranked.push_back(entry);
    }
    std::ranges::sort(ranked, [](const auto &left, const auto &right) {
      if (left.second.first != right.second.first) {
        return left.second.first > right.second.first;
      }
      return left.second.second < right.second.second;
    });

    wh::retriever::retriever_response fused{};
    fused.reserve(ranked.size());
    for (const auto &entry : ranked) {
      auto document = documents.at(entry.first);
      document.with_score(entry.second.first);
      fused.push_back(std::move(document));
    }
    return fused;
  }
};

template <retriever_component retriever_t>
struct named_retriever {
  std::string name{};
  retriever_t retriever;
};

} // namespace router_detail

/// Router flow over one homogeneous retriever set.
template <router_detail::retriever_component retriever_t,
          router_detail::route_policy route_t = router_detail::route_all_retrievers,
          router_detail::fusion_policy fusion_t = router_detail::reciprocal_rank_fusion>
class router {
public:
  router() = default;

  explicit router(route_t route_policy, fusion_t fusion_policy = {}) noexcept
      : route_policy_(std::move(route_policy)),
        fusion_policy_(std::move(fusion_policy)) {}

  /// Registers one named retriever before freeze.
  auto add_retriever(std::string name, retriever_t retriever)
      -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (name.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (name_index_.contains(name)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }
    name_index_.insert(name);
    retrievers_.push_back(router_detail::named_retriever<retriever_t>{
        .name = std::move(name), .retriever = std::move(retriever)});
    return {};
  }

  /// Freezes the current retriever-name snapshot.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (retrievers_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    frozen_names_.clear();
    frozen_names_.reserve(retrievers_.size());
    for (const auto &entry : retrievers_) {
      frozen_names_.push_back(entry.name);
    }
    frozen_ = true;
    return {};
  }

  /// Runs routing and fusion for one retrieval request.
  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request,
                              wh::core::run_context &context) const
      -> wh::core::result<wh::retriever::retriever_response> {
    auto self = const_cast<router *>(this);
    auto frozen = self->freeze();
    if (frozen.has_error()) {
      return wh::core::result<wh::retriever::retriever_response>::failure(
          frozen.error());
    }

    auto selected = route_policy_(request, frozen_names_);
    if (selected.has_error()) {
      return wh::core::result<wh::retriever::retriever_response>::failure(
          selected.error());
    }
    if (selected.value().empty()) {
      return wh::core::result<wh::retriever::retriever_response>::failure(
          wh::core::errc::not_found);
    }

    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        selected_names{selected.value().begin(), selected.value().end()};
    for (const auto &name : selected_names) {
      if (!name_index_.contains(name)) {
        return wh::core::result<wh::retriever::retriever_response>::failure(
            wh::core::errc::not_found);
      }
    }

    std::vector<routed_retriever_result> results{};
    for (const auto &entry : retrievers_) {
      if (!selected_names.contains(entry.name)) {
        continue;
      }
      auto status = entry.retriever.retrieve(request, context);
      if (status.has_error()) {
        return wh::core::result<wh::retriever::retriever_response>::failure(
            status.error());
      }
      results.push_back(routed_retriever_result{
          .retriever_name = entry.name,
          .documents = std::move(status).value(),
      });
    }
    return fusion_policy_(results);
  }

private:
  /// Registered retrievers in stable declaration order.
  std::vector<router_detail::named_retriever<retriever_t>> retrievers_{};
  /// Unique-name guard used during authoring.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      name_index_{};
  /// Frozen retriever name snapshot used by the default route policy.
  std::vector<std::string> frozen_names_{};
  /// Route-selection policy.
  route_t route_policy_{};
  /// Result-fusion policy.
  fusion_t fusion_policy_{};
  /// True after the route-name snapshot has been frozen.
  bool frozen_{false};
};

} // namespace wh::flow::retriever
