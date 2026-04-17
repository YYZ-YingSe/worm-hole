#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "wh/flow/indexing/parent.hpp"

namespace {

struct batch_indexer_impl {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"BatchIndexer", wh::core::component_kind::indexer};
  }

  [[nodiscard]] auto write(const wh::indexer::indexer_request &request) const
      -> wh::core::result<wh::indexer::indexer_response> {
    wh::indexer::indexer_response response{};
    response.success_count = request.documents.size();
    for (std::size_t index = 0; index < request.documents.size(); ++index) {
      response.document_ids.push_back("doc-" + std::to_string(index));
    }
    return response;
  }
};

} // namespace

TEST_CASE("indexing parent flow derives parent ids and writes transformed chunks",
          "[UT][wh/flow/indexing/parent.hpp][parent::write][branch][boundary]") {
  wh::flow::indexing::parent flow{
      wh::indexer::indexer{batch_indexer_impl{}},
      [](const wh::schema::document &document, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        return std::vector<wh::schema::document>{
            wh::schema::document{document.content() + "/0"},
            wh::schema::document{document.content() + "/1"}};
      },
      [](const wh::schema::document &, const std::size_t count,
         wh::core::run_context &)
          -> wh::core::result<std::vector<std::string>> {
        std::vector<std::string> ids{};
        for (std::size_t index = 0; index < count; ++index) {
          ids.push_back("sub-" + std::to_string(index));
        }
        return ids;
      }};

  wh::schema::document parent_document{"parent"};
  parent_document.set_metadata(std::string{wh::flow::indexing::parent_id_metadata_key},
                               "pid");
  REQUIRE(wh::flow::indexing::detail::indexing::effective_parent_id(parent_document) ==
          "pid");

  wh::indexer::indexer_request request{};
  request.documents.push_back(std::move(parent_document));
  wh::core::run_context context{};

  auto unfrozen = stdexec::sync_wait(flow.write(request, context));
  REQUIRE(unfrozen.has_value());
  REQUIRE(std::get<0>(*unfrozen).has_error());
  REQUIRE(std::get<0>(*unfrozen).error() == wh::core::errc::contract_violation);

  REQUIRE(flow.freeze().has_value());
  auto awaited = stdexec::sync_wait(flow.write(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().success_count == 2U);
  REQUIRE(flow.frozen());
  STATIC_REQUIRE(std::is_copy_constructible_v<decltype(flow)>);
  REQUIRE(flow.descriptor().kind == wh::core::component_kind::indexer);
}

TEST_CASE("indexing parent flow rejects empty transforms and id count mismatch",
          "[UT][wh/flow/indexing/parent.hpp][parent::freeze][branch]") {
  wh::indexer::indexer indexer{batch_indexer_impl{}};
  wh::core::run_context context{};
  wh::indexer::indexer_request request{};
  request.documents.push_back(wh::schema::document{"parent"});

  wh::flow::indexing::parent empty_transform{
      indexer,
      [](const wh::schema::document &, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        return std::vector<wh::schema::document>{};
      },
      [](const wh::schema::document &, const std::size_t,
         wh::core::run_context &) -> wh::core::result<std::vector<std::string>> {
        return std::vector<std::string>{};
      }};
  REQUIRE(empty_transform.freeze().has_value());
  auto empty_waited = stdexec::sync_wait(empty_transform.write(request, context));
  REQUIRE(empty_waited.has_value());
  REQUIRE(std::get<0>(*empty_waited).has_error());
  REQUIRE(std::get<0>(*empty_waited).error() == wh::core::errc::invalid_argument);

  wh::flow::indexing::parent mismatched_ids{
      indexer,
      [](const wh::schema::document &document, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        return std::vector<wh::schema::document>{
            wh::schema::document{document.content() + "/0"},
            wh::schema::document{document.content() + "/1"}};
      },
      [](const wh::schema::document &, const std::size_t,
         wh::core::run_context &) -> wh::core::result<std::vector<std::string>> {
        return std::vector<std::string>{"only-one"};
      }};
  REQUIRE(mismatched_ids.freeze().has_value());
  auto mismatched_waited = stdexec::sync_wait(mismatched_ids.write(request, context));
  REQUIRE(mismatched_waited.has_value());
  REQUIRE(std::get<0>(*mismatched_waited).has_error());
  REQUIRE(std::get<0>(*mismatched_waited).error() == wh::core::errc::invalid_argument);
}

TEST_CASE("indexing parent helper falls back to document content when parent id metadata is absent",
          "[UT][wh/flow/indexing/parent.hpp][effective_parent_id][condition][boundary]") {
  wh::schema::document explicit_parent{"content-a"};
  explicit_parent.set_metadata(
      std::string{wh::flow::indexing::parent_id_metadata_key}, "parent-a");
  REQUIRE(
      wh::flow::indexing::detail::indexing::effective_parent_id(explicit_parent) ==
      "parent-a");

  wh::schema::document fallback_parent{"content-b"};
  REQUIRE(
      wh::flow::indexing::detail::indexing::effective_parent_id(fallback_parent) ==
      "content-b");
}
