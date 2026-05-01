#include <algorithm>
#include <tuple>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/flow/retrieval/multi_query.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

namespace {

struct query_retriever_impl {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"QueryRetriever", wh::core::component_kind::retriever};
  }

  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request) const
      -> wh::core::result<wh::retriever::retriever_response> {
    return wh::retriever::retriever_response{wh::schema::document{"doc:" + request.query}};
  }
};

struct rewrite_model_impl {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"RewriteModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &) const
      -> wh::model::chat_invoke_result {
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"alpha\nalpha\nbeta"});
    return wh::model::chat_response{message, {}};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &) const
      -> wh::model::chat_message_stream_result {
    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
            wh::schema::message{.role = wh::schema::message_role::assistant,
                                .parts = {wh::schema::text_part{"unused"}}})};
  }
};

} // namespace

TEST_CASE("multi query helpers expose stable keys and rewrite helpers",
          "[UT][wh/flow/retrieval/multi_query.hpp][render_message_text][branch][boundary]") {
  wh::schema::message message{};
  message.parts.emplace_back(wh::schema::text_part{"alpha"});
  message.parts.emplace_back(wh::schema::text_part{"beta"});
  REQUIRE(wh::flow::retrieval::detail::multi_query::render_message_text(message) == "alphabeta");

  wh::flow::retrieval::detail::multi_query::original_query_rewriter original{};
  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto original_queries = original(request, context);
  REQUIRE(original_queries.has_value());
  REQUIRE(original_queries.value() == std::vector<std::string>{"hello"});

  wh::flow::retrieval::detail::multi_query::chat_query_rewriter rewriter{
      wh::model::chat_model{rewrite_model_impl{}}};
  auto rewritten = rewriter(request, context);
  REQUIRE(rewritten.has_value());
  REQUIRE(rewritten.value() == std::vector<std::string>{"alpha", "alpha", "beta"});

  REQUIRE(wh::flow::retrieval::detail::multi_query::request_node_key() == "multi_query_request");
  REQUIRE(wh::flow::retrieval::detail::multi_query::rewrite_node_key() == "multi_query_rewrite");
  REQUIRE(wh::flow::retrieval::detail::multi_query::batch_node_key() == "multi_query_batch");
  REQUIRE(wh::flow::retrieval::detail::multi_query::tools_node_key() == "multi_query_tools");
  REQUIRE(wh::flow::retrieval::detail::multi_query::fusion_node_key() == "multi_query_fusion");
  REQUIRE(wh::flow::retrieval::detail::multi_query::tool_name() == "multi_query_retrieve");
}

TEST_CASE("multi query flow rewrites clips and fuses retrieved documents",
          "[UT][wh/flow/retrieval/multi_query.hpp][multi_query::retrieve][branch][boundary]") {
  auto rewriter = [](const wh::retriever::retriever_request &,
                     wh::core::run_context &) -> wh::core::result<std::vector<std::string>> {
    return std::vector<std::string>{"hello", "alpha", "alpha", "beta", "gamma"};
  };

  wh::flow::retrieval::multi_query mq{wh::retriever::retriever{query_retriever_impl{}}, rewriter};
  REQUIRE(mq.set_max_queries(0U).has_value());
  STATIC_REQUIRE(std::is_copy_constructible_v<decltype(mq)>);
  REQUIRE(mq.descriptor().kind == wh::core::component_kind::retriever);

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto unfrozen = stdexec::sync_wait(mq.retrieve(request, context));
  REQUIRE(unfrozen.has_value());
  REQUIRE(std::get<0>(*unfrozen).has_error());
  REQUIRE(std::get<0>(*unfrozen).error() == wh::core::errc::contract_violation);

  REQUIRE(mq.freeze().has_value());
  auto awaited = stdexec::sync_wait(mq.retrieve(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  const auto &documents = std::get<0>(*awaited).value();
  REQUIRE(documents.size() == 4U);
  REQUIRE(documents.front().content() == "doc:hello");
  REQUIRE(
      std::find_if(documents.begin(), documents.end(), [](const wh::schema::document &document) {
        return document.content() == "doc:alpha";
      }) != documents.end());
  REQUIRE(mq.frozen());
}

TEST_CASE("multi query flow propagates rewrite and fusion failures",
          "[UT][wh/flow/retrieval/multi_query.hpp][multi_query::freeze][branch]") {
  auto failing_rewriter =
      [](const wh::retriever::retriever_request &,
         wh::core::run_context &) -> wh::core::result<std::vector<std::string>> {
    return wh::core::result<std::vector<std::string>>::failure(wh::core::errc::network_error);
  };
  wh::flow::retrieval::multi_query failing_rewrite{wh::retriever::retriever{query_retriever_impl{}},
                                                   failing_rewriter};
  REQUIRE(failing_rewrite.freeze().has_value());

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto rewrite_waited = stdexec::sync_wait(failing_rewrite.retrieve(request, context));
  REQUIRE(rewrite_waited.has_value());
  REQUIRE(std::get<0>(*rewrite_waited).has_error());
  REQUIRE(std::get<0>(*rewrite_waited).error() == wh::core::errc::network_error);

  auto original = wh::flow::retrieval::detail::multi_query::original_query_rewriter{};
  auto failing_fusion = [](const std::vector<wh::flow::retrieval::query_retrieval> &)
      -> wh::core::result<wh::retriever::retriever_response> {
    return wh::core::result<wh::retriever::retriever_response>::failure(
        wh::core::errc::protocol_error);
  };
  wh::flow::retrieval::multi_query failing_merge{wh::retriever::retriever{query_retriever_impl{}},
                                                 original, failing_fusion};
  REQUIRE(failing_merge.freeze().has_value());
  auto fusion_waited = stdexec::sync_wait(failing_merge.retrieve(request, context));
  REQUIRE(fusion_waited.has_value());
  REQUIRE(std::get<0>(*fusion_waited).has_error());
  REQUIRE(std::get<0>(*fusion_waited).error() == wh::core::errc::protocol_error);
}

TEST_CASE(
    "multi query flow clips expanded queries and rejects empty rewrite output",
    "[UT][wh/flow/retrieval/multi_query.hpp][multi_query::set_max_queries][condition][boundary]") {
  auto repeated_rewriter =
      [](const wh::retriever::retriever_request &,
         wh::core::run_context &) -> wh::core::result<std::vector<std::string>> {
    return std::vector<std::string>{"hello", "alpha", "beta", "gamma"};
  };
  wh::flow::retrieval::multi_query clipped{wh::retriever::retriever{query_retriever_impl{}},
                                           repeated_rewriter};
  REQUIRE(clipped.set_max_queries(2U).has_value());
  REQUIRE(clipped.freeze().has_value());

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto clipped_waited = stdexec::sync_wait(clipped.retrieve(request, context));
  REQUIRE(clipped_waited.has_value());
  REQUIRE(std::get<0>(*clipped_waited).has_value());
  REQUIRE(std::get<0>(*clipped_waited).value().size() == 2U);

  auto empty_rewriter = [](const wh::retriever::retriever_request &,
                           wh::core::run_context &) -> wh::core::result<std::vector<std::string>> {
    return std::vector<std::string>{};
  };
  wh::flow::retrieval::multi_query empty_queries{wh::retriever::retriever{query_retriever_impl{}},
                                                 empty_rewriter};
  REQUIRE(empty_queries.set_max_queries(0U).has_value());
  REQUIRE(empty_queries.freeze().has_value());
  auto empty_waited = stdexec::sync_wait(empty_queries.retrieve(request, context));
  REQUIRE(empty_waited.has_value());
  REQUIRE(std::get<0>(*empty_waited).has_value());
  REQUIRE(std::get<0>(*empty_waited).value().size() == 1U);
  REQUIRE(std::get<0>(*empty_waited).value().front().content() == "doc:hello");
}
