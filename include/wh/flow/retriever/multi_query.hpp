// Defines multi-query retrieval flow on top of the existing retriever and
// model contracts.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/flow/agent/utils.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/message.hpp"

namespace wh::flow::retriever {

/// One query-local retrieval result.
struct query_retrieval {
  /// Expanded query text.
  std::string query{};
  /// Retrieved documents for this expanded query.
  wh::retriever::retriever_response documents{};
};

namespace multi_query_detail {

template <typename retriever_t>
concept retriever_component =
    requires(const retriever_t &retriever,
             const wh::retriever::retriever_request &request,
             wh::core::run_context &context) {
      { retriever.retrieve(request, context) }
      -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
    };

template <typename rewrite_t>
concept query_rewriter =
    requires(rewrite_t rewrite, const wh::retriever::retriever_request &request,
             wh::core::run_context &context) {
      { rewrite(request, context) }
      -> std::same_as<wh::core::result<std::vector<std::string>>>;
    };

template <typename fusion_t>
concept multi_query_fusion =
    requires(fusion_t fusion, const std::vector<query_retrieval> &results) {
      { fusion(results) } -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
    };

struct original_query_rewriter {
  [[nodiscard]] auto operator()(const wh::retriever::retriever_request &request,
                                wh::core::run_context &) const
      -> wh::core::result<std::vector<std::string>> {
    return std::vector<std::string>{request.query};
  }
};

struct first_hit_fusion {
  [[nodiscard]] auto operator()(const std::vector<query_retrieval> &results) const
      -> wh::core::result<wh::retriever::retriever_response> {
    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        seen{};
    wh::retriever::retriever_response fused{};
    for (const auto &result : results) {
      for (const auto &document : result.documents) {
        if (!seen.insert(document.content()).second) {
          continue;
        }
        fused.push_back(document);
      }
    }
    return fused;
  }
};

template <wh::model::chat_model_like model_t>
class chat_query_rewriter {
public:
  explicit chat_query_rewriter(model_t model) noexcept : model_(std::move(model)) {}

  [[nodiscard]] auto operator()(const wh::retriever::retriever_request &request,
                                wh::core::run_context &context) const
      -> wh::core::result<std::vector<std::string>> {
    wh::model::chat_request rewrite_request{};
    wh::schema::message message{};
    message.role = wh::schema::message_role::user;
    message.parts.emplace_back(wh::schema::text_part{
        "Rewrite the query into up to 5 search variants, one per line:\n" +
        request.query});
    rewrite_request.messages.push_back(std::move(message));

    auto status = model_.invoke(rewrite_request, context);
    if (status.has_error()) {
      return wh::core::result<std::vector<std::string>>::failure(status.error());
    }
    std::vector<std::string> queries{};
    std::istringstream lines{
        wh::flow::agent::render_message_text(status.value().message)};
    std::string line{};
    while (std::getline(lines, line)) {
      line.erase(line.begin(),
                 std::find_if(line.begin(), line.end(), [](unsigned char ch) {
                   return std::isspace(ch) == 0;
                 }));
      while (!line.empty() &&
             std::isspace(static_cast<unsigned char>(line.back())) != 0) {
        line.pop_back();
      }
      if (!line.empty()) {
        queries.push_back(std::move(line));
      }
    }
    return queries;
  }

private:
  model_t model_;
};

} // namespace multi_query_detail

/// Multi-query retrieval flow over one retriever-like component.
template <multi_query_detail::retriever_component retriever_t,
          multi_query_detail::query_rewriter rewrite_t =
              multi_query_detail::original_query_rewriter,
          multi_query_detail::multi_query_fusion fusion_t =
              multi_query_detail::first_hit_fusion>
class multi_query {
public:
  multi_query(retriever_t retriever, rewrite_t rewriter = {},
              fusion_t fusion = {}) noexcept
      : retriever_(std::move(retriever)), rewriter_(std::move(rewriter)),
        fusion_(std::move(fusion)) {}

  /// Replaces the maximum query fanout. Zero falls back to 5.
  auto set_max_queries(const std::size_t max_queries) noexcept -> void {
    max_queries_ = max_queries == 0U ? 5U : max_queries;
  }

  /// Runs query rewrite, parallel-free stable expansion, and final fusion.
  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request,
                              wh::core::run_context &context) const
      -> wh::core::result<wh::retriever::retriever_response> {
    auto rewritten = rewriter_(request, context);
    if (rewritten.has_error()) {
      return wh::core::result<wh::retriever::retriever_response>::failure(
          rewritten.error());
    }

    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        seen_queries{};
    std::vector<std::string> queries{};
    const auto push_query = [&](const std::string &query) -> void {
      if (query.empty() || queries.size() >= max_queries_) {
        return;
      }
      if (seen_queries.insert(query).second) {
        queries.push_back(query);
      }
    };

    push_query(request.query);
    for (const auto &query : rewritten.value()) {
      push_query(query);
    }
    if (queries.empty()) {
      return wh::core::result<wh::retriever::retriever_response>::failure(
          wh::core::errc::not_found);
    }

    std::vector<query_retrieval> results{};
    results.reserve(queries.size());
    for (const auto &query : queries) {
      auto query_request = request;
      query_request.query = query;
      auto status = retriever_.retrieve(query_request, context);
      if (status.has_error()) {
        return wh::core::result<wh::retriever::retriever_response>::failure(
            status.error());
      }
      results.push_back(query_retrieval{
          .query = query,
          .documents = std::move(status).value(),
      });
    }
    return fusion_(results);
  }

private:
  retriever_t retriever_;
  rewrite_t rewriter_;
  fusion_t fusion_;
  std::size_t max_queries_{5U};
};

} // namespace wh::flow::retriever
