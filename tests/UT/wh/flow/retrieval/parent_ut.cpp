#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "wh/flow/retrieval/parent.hpp"

namespace {

struct child_retriever_impl {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"ChildRetriever", wh::core::component_kind::retriever};
  }

  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request) const
      -> wh::core::result<wh::retriever::retriever_response> {
    wh::schema::document first{request.query + "-1"};
    first.set_metadata(std::string{wh::flow::retrieval::parent_id_metadata_key},
                       "parent-a");
    wh::schema::document duplicate{"dup"};
    duplicate.set_metadata(std::string{wh::flow::retrieval::parent_id_metadata_key},
                           "parent-a");
    wh::schema::document second{request.query + "-2"};
    second.set_metadata(std::string{wh::flow::retrieval::parent_id_metadata_key},
                        "parent-b");
    return wh::retriever::retriever_response{
        std::move(first), std::move(duplicate), std::move(second)};
  }
};

} // namespace

TEST_CASE("retrieval parent flow loads unique parent documents in child order",
          "[UT][wh/flow/retrieval/parent.hpp][parent::retrieve][branch][boundary]") {
  wh::flow::retrieval::parent flow{
      wh::retriever::retriever{child_retriever_impl{}},
      [](const std::vector<std::string> &parent_ids, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        std::vector<wh::schema::document> documents{};
        for (const auto &parent_id : parent_ids) {
          wh::schema::document document{"doc:" + parent_id};
          document.set_metadata(std::string{wh::flow::retrieval::parent_id_metadata_key},
                                parent_id);
          documents.push_back(std::move(document));
        }
        return documents;
      }};
  STATIC_REQUIRE(std::is_copy_constructible_v<decltype(flow)>);
  REQUIRE(flow.descriptor().kind == wh::core::component_kind::retriever);

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};

  auto awaited = stdexec::sync_wait(flow.retrieve(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().size() == 2U);
  REQUIRE(std::get<0>(*awaited).value()[0].content() == "doc:parent-a");
  REQUIRE(std::get<0>(*awaited).value()[1].content() == "doc:parent-b");
  REQUIRE(flow.frozen());
}

TEST_CASE("retrieval parent flow propagates parent loader failures",
          "[UT][wh/flow/retrieval/parent.hpp][parent::freeze][branch]") {
  wh::flow::retrieval::parent flow{
      wh::retriever::retriever{child_retriever_impl{}},
      [](const std::vector<std::string> &, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        return wh::core::result<std::vector<wh::schema::document>>::failure(
            wh::core::errc::network_error);
      }};

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(flow.retrieve(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_error());
  REQUIRE(std::get<0>(*awaited).error() == wh::core::errc::network_error);
}

TEST_CASE("retrieval parent helpers reject type mismatches and freeze idempotently",
          "[UT][wh/flow/retrieval/parent.hpp][detail::parent::read_graph_value][condition][branch][boundary]") {
  auto mismatch = wh::flow::retrieval::detail::parent::read_graph_value<
      wh::retriever::retriever_response>(wh::compose::graph_value{42});
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);

  wh::flow::retrieval::parent flow{
      wh::retriever::retriever{child_retriever_impl{}},
      [](const std::vector<std::string> &, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        return std::vector<wh::schema::document>{};
      }};

  auto first_freeze = flow.freeze();
  REQUIRE(first_freeze.has_value());
  REQUIRE(flow.frozen());
  auto second_freeze = flow.freeze();
  REQUIRE(second_freeze.has_value());
}
