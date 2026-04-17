#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/flow.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/document.hpp"

namespace {

template <typename fn_t> struct sync_retriever_impl {
  fn_t fn;

  [[nodiscard]] auto retrieve(
      const wh::retriever::retriever_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t>
sync_retriever_impl(fn_t) -> sync_retriever_impl<fn_t>;

template <typename fn_t> struct sync_indexer_impl {
  fn_t fn;

  [[nodiscard]] auto write(const wh::indexer::indexer_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t>
sync_indexer_impl(fn_t) -> sync_indexer_impl<fn_t>;

[[nodiscard]] auto make_retriever(std::string prefix) {
  return wh::retriever::retriever{sync_retriever_impl{
      [prefix = std::move(prefix)](const wh::retriever::retriever_request &request)
          -> wh::core::result<wh::retriever::retriever_response> {
        return wh::retriever::retriever_response{
            wh::schema::document{prefix + request.query}};
      }}};
}

[[nodiscard]] auto make_indexer() {
  return wh::indexer::indexer{sync_indexer_impl{
      [](const wh::indexer::indexer_request &request)
          -> wh::core::result<wh::indexer::indexer_response> {
        wh::indexer::indexer_response response{};
        response.success_count = request.documents.size();
        for (std::size_t index = 0U; index < request.documents.size(); ++index) {
          response.document_ids.push_back("sub-" + std::to_string(index));
        }
        return response;
      }}};
}

} // namespace

TEST_CASE("flow public umbrella exposes retrieval and indexing facades through one header",
          "[core][flow][functional]") {
  wh::core::run_context context{};

  wh::retriever::retriever_request retrieve_request{};
  retrieve_request.query = "hello";
  auto multi_query = wh::flow::retrieval::multi_query{make_retriever("mq:")};
  REQUIRE(multi_query.set_max_queries(2U).has_value());
  REQUIRE(multi_query.freeze().has_value());

  auto retrieved_waited =
      stdexec::sync_wait(multi_query.retrieve(retrieve_request, context));
  REQUIRE(retrieved_waited.has_value());
  auto retrieved_status = std::move(std::get<0>(retrieved_waited.value()));
  REQUIRE(retrieved_status.has_value());
  REQUIRE(retrieved_status.value().size() == 1U);
  REQUIRE(retrieved_status.value().front().content() == "mq:hello");

  wh::indexer::indexer_request index_request{};
  index_request.documents.push_back(wh::schema::document{"parent-doc"});
  auto parent = wh::flow::indexing::parent{
      make_indexer(),
      [](const wh::schema::document &document, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        return std::vector<wh::schema::document>{
            wh::schema::document{document.content() + "/chunk"}};
      },
      [](const wh::schema::document &, const std::size_t count,
         wh::core::run_context &)
          -> wh::core::result<std::vector<std::string>> {
        std::vector<std::string> ids{};
        for (std::size_t index = 0U; index < count; ++index) {
          ids.push_back("sub-" + std::to_string(index));
        }
        return ids;
      }};
  REQUIRE(parent.freeze().has_value());

  auto indexed_waited = stdexec::sync_wait(parent.write(index_request, context));
  REQUIRE(indexed_waited.has_value());
  auto indexed_status = std::move(std::get<0>(indexed_waited.value()));
  REQUIRE(indexed_status.has_value());
  REQUIRE(indexed_status.value().success_count == 1U);
  REQUIRE(indexed_status.value().document_ids == std::vector<std::string>{"sub-0"});
}
