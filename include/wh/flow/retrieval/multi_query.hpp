// Defines multi-query retrieval as one retriever-like direct wrapper.
#pragma once

#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/concurrent_sender_vector.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/message.hpp"

namespace wh::flow::retrieval {

/// One query-local retrieval result.
struct query_retrieval {
  /// Expanded query text.
  std::string query{};
  /// Retrieved documents for this expanded query.
  wh::retriever::retriever_response documents{};
};

namespace detail::multi_query {

using callback_sink = wh::callbacks::callback_sink;

struct rewrite_state {
  /// Base request copied once at rewrite boundary for later query fan-out.
  wh::retriever::retriever_request base_request{};
  /// Stable expanded queries after trimming, de-duplication, and clipping.
  std::vector<std::string> queries{};
};

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
      { fusion(results) }
      -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
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

[[nodiscard]] inline auto render_message_text(const wh::schema::message &message)
    -> std::string {
  std::string rendered{};
  for (const auto &part : message.parts) {
    if (const auto *text = std::get_if<wh::schema::text_part>(&part);
        text != nullptr) {
      rendered.append(text->text);
    }
  }
  return rendered;
}

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
    std::istringstream lines{render_message_text(status.value().message)};
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

[[nodiscard]] inline auto request_node_key() -> std::string_view {
  return "multi_query_request";
}

[[nodiscard]] inline auto rewrite_node_key() -> std::string_view {
  return "multi_query_rewrite";
}

[[nodiscard]] inline auto batch_node_key() -> std::string_view {
  return "multi_query_batch";
}

[[nodiscard]] inline auto tools_node_key() -> std::string_view {
  return "multi_query_tools";
}

[[nodiscard]] inline auto fusion_node_key() -> std::string_view {
  return "multi_query_fusion";
}

[[nodiscard]] inline auto output_key() -> std::string_view {
  return "documents";
}

[[nodiscard]] inline auto tool_name() -> std::string_view {
  return "multi_query_retrieve";
}

template <typename rewrite_t>
[[nodiscard]] inline auto build_rewrite_state(
    const rewrite_t &rewriter, const wh::retriever::retriever_request &request,
    const std::size_t max_queries, wh::core::run_context &context)
    -> wh::core::result<rewrite_state> {
  auto rewritten = rewriter(request, context);
  if (rewritten.has_error()) {
    return wh::core::result<rewrite_state>::failure(rewritten.error());
  }

  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      seen_queries{};
  rewrite_state state{};
  state.base_request = request;
  state.queries.reserve(max_queries);

  const auto push_query = [&](const std::string &query) -> void {
    if (query.empty() || state.queries.size() >= max_queries) {
      return;
    }
    if (seen_queries.insert(query).second) {
      state.queries.push_back(query);
    }
  };

  push_query(request.query);
  for (const auto &query : rewritten.value()) {
    push_query(query);
  }
  if (state.queries.empty()) {
    return wh::core::result<rewrite_state>::failure(wh::core::errc::not_found);
  }
  return state;
}

template <retriever_component retriever_t>
[[nodiscard]] inline auto make_query_sender(
    const retriever_t &retriever, const std::string &query,
    const wh::retriever::retriever_request &base_request,
    wh::core::run_context &context) {
  using query_status = wh::core::result<query_retrieval>;
  auto request = base_request;
  request.query = query;

  if constexpr (requires(const retriever_t &value,
                         wh::retriever::retriever_request &&owned_request,
                         wh::core::run_context &run_context) {
                  { value.async_retrieve(std::move(owned_request), run_context) }
                  -> stdexec::sender;
                }) {
    return wh::core::detail::map_result_sender<query_status>(
        wh::core::detail::normalize_result_sender<
            wh::core::result<wh::retriever::retriever_response>>(
            retriever.async_retrieve(std::move(request), context)),
        [query = std::string{query}](wh::retriever::retriever_response documents)
            mutable -> query_status {
          return query_retrieval{
              .query = std::move(query),
              .documents = std::move(documents),
          };
        });
  } else {
    auto status = retriever.retrieve(std::move(request), context);
    if (status.has_error()) {
      return stdexec::just(query_status::failure(status.error()));
    }
    return stdexec::just(query_status{query_retrieval{
        .query = std::string{query},
        .documents = std::move(status).value(),
    }});
  }
}

template <retriever_component retriever_t>
using query_sender_t = decltype(make_query_sender(
    std::declval<const retriever_t &>(), std::declval<const std::string &>(),
    std::declval<const wh::retriever::retriever_request &>(),
    std::declval<wh::core::run_context &>()));

template <typename fusion_t>
[[nodiscard]] inline auto fuse_query_results(
    const fusion_t &fusion, const rewrite_state &rewrite,
    std::vector<query_retrieval> query_results, wh::core::run_context &context)
    -> wh::core::result<wh::retriever::retriever_response> {
  auto sink = wh::callbacks::filter_callback_sink(
      wh::callbacks::borrow_callback_sink(context), rewrite.base_request.options);

  wh::retriever::retriever_callback_event event{};
  event.top_k = rewrite.base_request.options.resolve_view().top_k;
  event.score_threshold =
      rewrite.base_request.options.resolve_view().score_threshold;
  event.filter =
      std::string{rewrite.base_request.options.resolve_view().filter};
  event.extra = std::to_string(query_results.size());

  wh::callbacks::run_info run_info{};
  run_info.name = "FusionFunc";
  run_info.type = "FusionFunc";
  run_info.component = wh::core::component_kind::retriever;
  run_info = wh::callbacks::apply_component_run_info(
      std::move(run_info), rewrite.base_request.options);
  wh::callbacks::emit(sink, wh::callbacks::stage::start, event, run_info);

  auto fused = fusion(query_results);
  if (fused.has_error()) {
    wh::callbacks::emit(sink, wh::callbacks::stage::error, event, run_info);
    return wh::core::result<wh::retriever::retriever_response>::failure(
        fused.error());
  }

  event.extra = std::to_string(fused.value().size());
  wh::callbacks::emit(sink, wh::callbacks::stage::end, event, run_info);
  return fused;
}

template <retriever_component retriever_t, query_rewriter rewrite_t,
          multi_query_fusion fusion_t>
struct authored_state {
  authored_state(retriever_t retriever, rewrite_t rewriter, fusion_t fusion) noexcept
      : retriever(std::move(retriever)), rewriter(std::move(rewriter)),
        fusion(std::move(fusion)) {}

  retriever_t retriever;
  rewrite_t rewriter;
  fusion_t fusion;
  std::size_t max_queries{5U};
};

template <retriever_component retriever_t, query_rewriter rewrite_t,
          multi_query_fusion fusion_t>
struct runtime_state {
  runtime_state(retriever_t retriever, rewrite_t rewriter, fusion_t fusion,
                const std::size_t max_queries) noexcept
      : retriever(std::move(retriever)), rewriter(std::move(rewriter)),
        fusion(std::move(fusion)), max_queries(max_queries) {}

  retriever_t retriever;
  rewrite_t rewriter;
  fusion_t fusion;
  std::size_t max_queries{5U};
};

template <retriever_component retriever_t, query_rewriter rewrite_t,
          multi_query_fusion fusion_t>
[[nodiscard]] inline auto make_fanout_sender(
    std::shared_ptr<const runtime_state<retriever_t, rewrite_t, fusion_t>> state,
    rewrite_state rewrite, wh::core::run_context &context) {
  using query_status = wh::core::result<query_retrieval>;
  using output_status = wh::core::result<wh::retriever::retriever_response>;
  using child_sender_t = query_sender_t<retriever_t>;

  std::vector<child_sender_t> senders{};
  senders.reserve(rewrite.queries.size());
  for (const auto &query : rewrite.queries) {
    senders.push_back(make_query_sender(state->retriever, query,
                                        rewrite.base_request, context));
  }

  return wh::core::detail::make_concurrent_sender_vector<query_status>(
             std::move(senders), rewrite.queries.size()) |
         stdexec::then([state, rewrite = std::move(rewrite), &context](
                           std::vector<query_status> statuses) mutable
                           -> output_status {
           std::vector<query_retrieval> query_results{};
           query_results.reserve(statuses.size());
           for (auto &status : statuses) {
             if (status.has_error()) {
               return output_status::failure(status.error());
             }
             query_results.push_back(std::move(status).value());
           }
           return fuse_query_results(state->fusion, rewrite,
                                     std::move(query_results), context);
         });
}

template <retriever_component retriever_t, query_rewriter rewrite_t,
          multi_query_fusion fusion_t>
[[nodiscard]] inline auto make_pipeline_sender(
    std::shared_ptr<const runtime_state<retriever_t, rewrite_t, fusion_t>> state,
    wh::retriever::retriever_request request, wh::core::run_context &context) {
  using rewrite_status = wh::core::result<rewrite_state>;
  using output_status = wh::core::result<wh::retriever::retriever_response>;
  using failure_sender_t =
      decltype(stdexec::just(output_status::failure(wh::core::errc::internal_error)));
  using fanout_sender_t = decltype(make_fanout_sender(
      std::declval<
          std::shared_ptr<const runtime_state<retriever_t, rewrite_t, fusion_t>>>(),
      std::declval<rewrite_state>(), std::declval<wh::core::run_context &>()));
  using dispatch_sender_t =
      wh::core::detail::variant_sender<failure_sender_t, fanout_sender_t>;

  auto rewrite_sender = stdexec::just() | stdexec::then(
      [state, request = std::move(request), &context]() mutable -> rewrite_status {
        return build_rewrite_state(state->rewriter, request, state->max_queries,
                                   context);
      });

  return stdexec::let_value(
      std::move(rewrite_sender),
      [state, &context](rewrite_status rewritten) mutable {
        if (rewritten.has_error()) {
          return dispatch_sender_t{
              stdexec::just(output_status::failure(rewritten.error()))};
        }
        return dispatch_sender_t{
            make_fanout_sender(state, std::move(rewritten).value(), context)};
      });
}

[[nodiscard]] inline auto map_multi_query_result_sender(auto &&sender) {
  return wh::core::detail::normalize_result_sender<
      wh::core::result<wh::retriever::retriever_response>>(
      std::forward<decltype(sender)>(sender));
}

} // namespace detail::multi_query

/// Multi-query retrieval flow over one retriever-like component.
template <detail::multi_query::retriever_component retriever_t,
          detail::multi_query::query_rewriter rewrite_t =
              detail::multi_query::original_query_rewriter,
          detail::multi_query::multi_query_fusion fusion_t =
              detail::multi_query::first_hit_fusion>
class multi_query {
  using authored_state_t =
      detail::multi_query::authored_state<retriever_t, rewrite_t, fusion_t>;
  using runtime_state_t =
      detail::multi_query::runtime_state<retriever_t, rewrite_t, fusion_t>;

public:
  multi_query(retriever_t retriever, rewrite_t rewriter = {},
              fusion_t fusion = {}) noexcept
      : authored_(std::in_place, std::move(retriever), std::move(rewriter),
                  std::move(fusion)) {}

  multi_query(const multi_query &) = default;
  auto operator=(const multi_query &) -> multi_query & = default;
  multi_query(multi_query &&) noexcept = default;
  auto operator=(multi_query &&) noexcept -> multi_query & = default;
  ~multi_query() = default;

  /// Returns component metadata when used as one retriever-like primitive.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"MultiQuery", wh::core::component_kind::retriever};
  }

  /// Returns true after the flow configuration has frozen successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return runtime_ != nullptr; }

  /// Replaces the maximum query fanout. Zero falls back to 5.
  auto set_max_queries(const std::size_t max_queries) -> wh::core::result<void> {
    if (!authored_.has_value() || runtime_ != nullptr) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    authored_->max_queries = max_queries == 0U ? 5U : max_queries;
    return {};
  }

  /// Freezes configuration for later direct wrapper execution.
  auto freeze() -> wh::core::result<void> {
    if (runtime_ != nullptr) {
      return {};
    }
    if (!authored_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (authored_->max_queries == 0U) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    runtime_ = std::make_shared<runtime_state_t>(
        std::move(authored_->retriever), std::move(authored_->rewriter),
        std::move(authored_->fusion), authored_->max_queries);
    authored_.reset();
    return {};
  }

  /// Runs query rewrite, concurrent retrieval, and final fusion on one frozen flow.
  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request,
                              wh::core::run_context &context) const {
    return async_retrieve(request, context);
  }

  /// Runs query rewrite, concurrent retrieval, and final fusion on one frozen flow.
  [[nodiscard]] auto retrieve(wh::retriever::retriever_request &&request,
                              wh::core::run_context &context) const {
    return async_retrieve(std::move(request), context);
  }

  /// Async component-node entry that forwards one frozen retriever-like sender.
  [[nodiscard]] auto async_retrieve(
      const wh::retriever::retriever_request &request,
      wh::core::run_context &context) const {
    return dispatch_request(wh::retriever::retriever_request{request}, context);
  }

  /// Async component-node entry that forwards one frozen retriever-like sender.
  [[nodiscard]] auto async_retrieve(wh::retriever::retriever_request &&request,
                                    wh::core::run_context &context) const {
    return dispatch_request(std::move(request), context);
  }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>,
                          wh::retriever::retriever_request>
  [[nodiscard]] auto dispatch_request(request_t &&request,
                                      wh::core::run_context &context) const {
    using output_status = wh::core::result<wh::retriever::retriever_response>;
    using failure_sender_t = decltype(
        wh::core::detail::failure_result_sender<output_status>(
            wh::core::errc::internal_error));
    using pipeline_sender_t = decltype(
        detail::multi_query::map_multi_query_result_sender(
            detail::multi_query::make_pipeline_sender(
                std::declval<std::shared_ptr<const runtime_state_t>>(),
                std::declval<wh::retriever::retriever_request>(),
                std::declval<wh::core::run_context &>())));
    using dispatch_sender_t =
        wh::core::detail::variant_sender<failure_sender_t, pipeline_sender_t>;

    if (runtime_ == nullptr) {
      return dispatch_sender_t{
          wh::core::detail::failure_result_sender<output_status>(
              wh::core::errc::contract_violation)};
    }

    return dispatch_sender_t{detail::multi_query::map_multi_query_result_sender(
        detail::multi_query::make_pipeline_sender(
            runtime_,
            wh::retriever::retriever_request{std::forward<request_t>(request)},
            context))};
  }

  std::optional<authored_state_t> authored_{};
  std::shared_ptr<const runtime_state_t> runtime_{};
};

} // namespace wh::flow::retrieval
