#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/graph.hpp"
#include "wh/core/error.hpp"
#include "wh/document/keys.hpp"
#include "wh/flow/indexing.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/schema/document.hpp"

namespace {

template <typename fn_t> struct sync_indexer_batch_impl {
  fn_t fn;

  [[nodiscard]] auto write(const wh::indexer::indexer_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t>
sync_indexer_batch_impl(fn_t) -> sync_indexer_batch_impl<fn_t>;

struct indexer_probe_state {
  std::vector<wh::schema::document> seen_documents{};
};

[[nodiscard]] auto make_indexer(
    std::shared_ptr<indexer_probe_state> probe_state = nullptr) {
  return wh::indexer::indexer{sync_indexer_batch_impl{
      [probe_state = std::move(probe_state)](
          const wh::indexer::indexer_request &request)
          -> wh::core::result<wh::indexer::indexer_response> {
        if (probe_state != nullptr) {
          probe_state->seen_documents = request.documents;
        }
        wh::indexer::indexer_response response{};
        response.success_count = request.documents.size();
        for (std::size_t index = 0U; index < request.documents.size(); ++index) {
          const auto *sub_id = request.documents[index].metadata_ptr<std::string>(
              wh::document::sub_id_metadata_key);
          response.document_ids.push_back(
              sub_id == nullptr ? "doc-" + std::to_string(index) : *sub_id);
        }
        return response;
      }}};
}

} // namespace

TEST_CASE("flow indexing assembler freezes under flow namespace",
          "[core][flow][indexing]") {
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
  REQUIRE(parent.frozen());
  REQUIRE(parent.descriptor().kind == wh::core::component_kind::indexer);
}

TEST_CASE("flow indexing parent can be used as one async indexer component node",
          "[core][flow][indexing][component]") {
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

  wh::compose::graph graph{};
  REQUIRE(graph
              .add_component<wh::compose::component_kind::indexer,
                             wh::compose::node_contract::value,
                             wh::compose::node_contract::value,
                             wh::compose::node_exec_mode::async>(
                  "parent", parent)
              .has_value());
  REQUIRE(graph.add_entry_edge("parent").has_value());
  REQUIRE(graph.add_exit_edge("parent").has_value());
  REQUIRE(graph.compile().has_value());

  wh::indexer::indexer_request request{};
  request.documents.push_back(wh::schema::document{"doc"});
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(
      graph.invoke(context, wh::compose::graph_invoke_request{
                                .input = wh::compose::graph_input::value(
                                    wh::core::any(request))}));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  auto *response = wh::core::any_cast<wh::indexer::indexer_response>(
      &std::get<0>(*awaited).value().output_status.value());
  REQUIRE(response != nullptr);
  REQUIRE(response->success_count == 1U);
}

TEST_CASE("flow indexing parent executes direct write path and annotates child documents",
          "[core][flow][indexing]") {
  auto probe_state = std::make_shared<indexer_probe_state>();
  auto parent = wh::flow::indexing::parent{
      make_indexer(probe_state),
      [](const wh::schema::document &document, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        return std::vector<wh::schema::document>{
            wh::schema::document{document.content() + "/chunk-0"},
            wh::schema::document{document.content() + "/chunk-1"}};
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

  wh::indexer::indexer_request request{};
  wh::schema::document parent_document{"parent-doc"};
  parent_document.set_metadata(std::string{wh::document::parent_id_metadata_key},
                               "parent-id");
  request.documents.push_back(std::move(parent_document));

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(parent.write(request, context));
  REQUIRE(waited.has_value());

  auto status = std::move(std::get<0>(waited.value()));
  REQUIRE(status.has_value());
  REQUIRE(parent.frozen());
  REQUIRE(status.value().success_count == 2U);
  REQUIRE(status.value().document_ids ==
          std::vector<std::string>{"sub-0", "sub-1"});

  REQUIRE(probe_state->seen_documents.size() == 2U);
  REQUIRE(probe_state->seen_documents[0].content() == "parent-doc/chunk-0");
  REQUIRE(probe_state->seen_documents[1].content() == "parent-doc/chunk-1");

  const auto *first_parent_id =
      probe_state->seen_documents[0].metadata_ptr<std::string>(
          wh::document::parent_id_metadata_key);
  REQUIRE(first_parent_id != nullptr);
  REQUIRE(*first_parent_id == "parent-id");

  const auto *first_sub_id =
      probe_state->seen_documents[0].metadata_ptr<std::string>(
          wh::document::sub_id_metadata_key);
  REQUIRE(first_sub_id != nullptr);
  REQUIRE(*first_sub_id == "sub-0");

  const auto *second_sub_id =
      probe_state->seen_documents[1].metadata_ptr<std::string>(
          wh::document::sub_id_metadata_key);
  REQUIRE(second_sub_id != nullptr);
  REQUIRE(*second_sub_id == "sub-1");
}
