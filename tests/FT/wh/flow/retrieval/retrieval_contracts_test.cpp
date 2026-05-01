#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/graph/detail/invoke.hpp"
#include "wh/compose/graph/invoke_types.hpp"
#include "wh/core/error.hpp"
#include "wh/document/keys.hpp"
#include "wh/flow/retrieval.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/document.hpp"

namespace {

template <typename fn_t> struct sync_retriever_impl {
  fn_t fn;

  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t> sync_retriever_impl(fn_t) -> sync_retriever_impl<fn_t>;

[[nodiscard]] auto make_retriever(const std::string &prefix) {
  return wh::retriever::retriever{
      sync_retriever_impl{[prefix](const wh::retriever::retriever_request &request)
                              -> wh::core::result<wh::retriever::retriever_response> {
        wh::schema::document document{prefix + request.query};
        document.set_metadata(std::string{wh::document::parent_id_metadata_key},
                              prefix + "_parent");
        return wh::retriever::retriever_response{std::move(document)};
      }}};
}

} // namespace

TEST_CASE("flow retrieval assemblers freeze under flow namespace", "[core][flow][retrieval]") {
  using retriever_t = decltype(make_retriever(std::string{}));

  auto router = wh::flow::retrieval::router<retriever_t>{};
  REQUIRE(router.add_retriever("left", make_retriever("left:")).has_value());
  REQUIRE(router.add_retriever("right", make_retriever("right:")).has_value());
  REQUIRE(router.freeze().has_value());
  REQUIRE(router.frozen());
  REQUIRE(router.descriptor().kind == wh::core::component_kind::retriever);

  auto multi_query = wh::flow::retrieval::multi_query{make_retriever("mq:")};
  REQUIRE(multi_query.set_max_queries(3U).has_value());
  REQUIRE(multi_query.freeze().has_value());
  REQUIRE(multi_query.frozen());
  REQUIRE(multi_query.descriptor().kind == wh::core::component_kind::retriever);

  auto parent = wh::flow::retrieval::parent{
      make_retriever("child:"),
      [](const std::vector<std::string> &parent_ids,
         wh::core::run_context &) -> wh::core::result<std::vector<wh::schema::document>> {
        std::vector<wh::schema::document> documents{};
        for (const auto &parent_id : parent_ids) {
          documents.emplace_back("parent:" + parent_id);
        }
        return documents;
      }};
  REQUIRE(parent.freeze().has_value());
  REQUIRE(parent.frozen());
  REQUIRE(parent.descriptor().kind == wh::core::component_kind::retriever);
}

TEST_CASE("flow retrieval assemblers execute under flow namespace", "[core][flow][retrieval]") {
  using retriever_t = decltype(make_retriever(std::string{}));

  wh::core::run_context context{};
  wh::retriever::retriever_request request{};
  request.query = "hello";

  auto router = wh::flow::retrieval::router<retriever_t>{};
  REQUIRE(router.add_retriever("left", make_retriever("left:")).has_value());
  REQUIRE(router.add_retriever("right", make_retriever("right:")).has_value());
  REQUIRE(router.freeze().has_value());
  auto router_waited = stdexec::sync_wait(router.retrieve(request, context));
  REQUIRE(router_waited.has_value());
  auto router_status = std::move(std::get<0>(router_waited.value()));
  REQUIRE(router_status.has_value());
  REQUIRE(router_status.value().size() == 2U);
  std::unordered_set<std::string> router_contents{};
  for (const auto &document : router_status.value()) {
    router_contents.insert(document.content());
  }
  REQUIRE(router_contents.contains("left:hello"));
  REQUIRE(router_contents.contains("right:hello"));

  auto multi_query = wh::flow::retrieval::multi_query{make_retriever("mq:")};
  REQUIRE(multi_query.set_max_queries(3U).has_value());
  REQUIRE(multi_query.freeze().has_value());
  auto multi_waited = stdexec::sync_wait(multi_query.retrieve(request, context));
  REQUIRE(multi_waited.has_value());
  auto multi_status = std::move(std::get<0>(multi_waited.value()));
  if (multi_status.has_error()) {
    INFO("multi_query retrieve error=" << multi_status.error().value());
  }
  REQUIRE(multi_status.has_value());
  REQUIRE(multi_status.value().size() == 1U);
  REQUIRE(multi_status.value().front().content() == "mq:hello");

  auto parent = wh::flow::retrieval::parent{
      make_retriever("child:"),
      [](const std::vector<std::string> &parent_ids,
         wh::core::run_context &) -> wh::core::result<std::vector<wh::schema::document>> {
        std::vector<wh::schema::document> documents{};
        for (const auto &parent_id : parent_ids) {
          wh::schema::document document{"parent:" + parent_id};
          document.set_metadata(std::string{wh::document::parent_id_metadata_key}, parent_id);
          documents.push_back(std::move(document));
        }
        return documents;
      }};
  REQUIRE(parent.freeze().has_value());
  auto parent_waited = stdexec::sync_wait(parent.retrieve(request, context));
  REQUIRE(parent_waited.has_value());
  auto parent_status = std::move(std::get<0>(parent_waited.value()));
  REQUIRE(parent_status.has_value());
  REQUIRE(parent_status.value().size() == 1U);
  REQUIRE(parent_status.value().front().content() == "parent:child:_parent");
}

TEST_CASE("flow router can be used as one async retriever component node",
          "[core][flow][retrieval][component]") {
  using retriever_t = decltype(make_retriever(std::string{}));

  auto router = wh::flow::retrieval::router<retriever_t>{};
  REQUIRE(router.add_retriever("left", make_retriever("left:")).has_value());
  REQUIRE(router.add_retriever("right", make_retriever("right:")).has_value());
  REQUIRE(router.freeze().has_value());

  wh::compose::graph graph{};
  REQUIRE(
      graph
          .add_component<wh::compose::component_kind::retriever, wh::compose::node_contract::value,
                         wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
              "router", std::move(router))
          .has_value());
  REQUIRE(graph.add_entry_edge("router").has_value());
  REQUIRE(graph.add_exit_edge("router").has_value());
  REQUIRE(graph.compile().has_value());

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(
      graph.invoke(context, wh::compose::graph_invoke_request{
                                .input = wh::compose::graph_input::value(wh::core::any(request))}));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().output_status.has_value());
  auto *documents = wh::core::any_cast<wh::retriever::retriever_response>(
      &std::get<0>(*awaited).value().output_status.value());
  REQUIRE(documents != nullptr);
  REQUIRE(documents->size() == 2U);
}

TEST_CASE("flow retrieval primitives can be used as async component nodes",
          "[core][flow][retrieval][component]") {
  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};

  {
    auto multi_query = wh::flow::retrieval::multi_query{make_retriever("mq:")};
    REQUIRE(multi_query.set_max_queries(2U).has_value());
    REQUIRE(multi_query.freeze().has_value());

    wh::compose::graph graph{};
    REQUIRE(graph
                .add_component<wh::compose::component_kind::retriever,
                               wh::compose::node_contract::value, wh::compose::node_contract::value,
                               wh::compose::node_exec_mode::async>("multi_query", multi_query)
                .has_value());
    REQUIRE(graph.add_entry_edge("multi_query").has_value());
    REQUIRE(graph.add_exit_edge("multi_query").has_value());
    REQUIRE(graph.compile().has_value());

    auto awaited = stdexec::sync_wait(graph.invoke(
        context, wh::compose::graph_invoke_request{
                     .input = wh::compose::graph_input::value(wh::core::any(request))}));
    REQUIRE(awaited.has_value());
    REQUIRE(std::get<0>(*awaited).has_value());
    auto *documents = wh::core::any_cast<wh::retriever::retriever_response>(
        &std::get<0>(*awaited).value().output_status.value());
    REQUIRE(documents != nullptr);
    REQUIRE(documents->size() == 1U);
  }

  {
    auto parent = wh::flow::retrieval::parent{
        make_retriever("child:"),
        [](const std::vector<std::string> &parent_ids,
           wh::core::run_context &) -> wh::core::result<std::vector<wh::schema::document>> {
          std::vector<wh::schema::document> documents{};
          for (const auto &parent_id : parent_ids) {
            wh::schema::document document{"parent:" + parent_id};
            document.set_metadata(std::string{wh::document::parent_id_metadata_key}, parent_id);
            documents.push_back(std::move(document));
          }
          return documents;
        }};
    REQUIRE(parent.freeze().has_value());

    wh::compose::graph graph{};
    REQUIRE(graph
                .add_component<wh::compose::component_kind::retriever,
                               wh::compose::node_contract::value, wh::compose::node_contract::value,
                               wh::compose::node_exec_mode::async>("parent", parent)
                .has_value());
    REQUIRE(graph.add_entry_edge("parent").has_value());
    REQUIRE(graph.add_exit_edge("parent").has_value());
    REQUIRE(graph.compile().has_value());

    auto awaited = stdexec::sync_wait(graph.invoke(
        context, wh::compose::graph_invoke_request{
                     .input = wh::compose::graph_input::value(wh::core::any(request))}));
    REQUIRE(awaited.has_value());
    REQUIRE(std::get<0>(*awaited).has_value());
    auto *documents = wh::core::any_cast<wh::retriever::retriever_response>(
        &std::get<0>(*awaited).value().output_status.value());
    REQUIRE(documents != nullptr);
    REQUIRE(documents->size() == 1U);
  }
}

TEST_CASE("flow router executes custom route and fusion policies under flow namespace",
          "[core][flow][retrieval]") {
  using retriever_t = decltype(make_retriever(std::string{}));
  const auto route_policy =
      [](const wh::retriever::retriever_request &,
         const std::vector<std::string> &) -> wh::core::result<std::vector<std::string>> {
    return std::vector<std::string>{"right"};
  };
  const auto fusion_policy =
      [](const std::vector<wh::flow::retrieval::routed_retriever_result> &results)
      -> wh::core::result<wh::retriever::retriever_response> {
    wh::retriever::retriever_response fused{};
    for (const auto &result : results) {
      fused.emplace_back("fused:" + result.retriever_name + ":" +
                         result.documents.front().content());
    }
    return fused;
  };
  using route_t = decltype(route_policy);
  using fusion_t = decltype(fusion_policy);

  auto router =
      wh::flow::retrieval::router<retriever_t, route_t, fusion_t>{route_policy, fusion_policy};
  REQUIRE(router.add_retriever("left", make_retriever("left:")).has_value());
  REQUIRE(router.add_retriever("right", make_retriever("right:")).has_value());
  REQUIRE(router.freeze().has_value());

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(router.retrieve(request, context));
  REQUIRE(waited.has_value());

  auto status = std::move(std::get<0>(waited.value()));
  REQUIRE(status.has_value());
  REQUIRE(status.value().size() == 1U);
  REQUIRE(status.value().front().content() == "fused:right:right:hello");
}

TEST_CASE("flow multi-query applies query clipping deduplication and custom fusion",
          "[core][flow][retrieval]") {
  using retriever_t = decltype(make_retriever(std::string{}));
  const auto rewriter = [](const wh::retriever::retriever_request &,
                           wh::core::run_context &) -> wh::core::result<std::vector<std::string>> {
    return std::vector<std::string>{"hello",   "hello-alt", "hello-alt", "hello-2",
                                    "hello-3", "hello-4",   "hello-5",   "hello-6"};
  };
  const auto fusion = [](const std::vector<wh::flow::retrieval::query_retrieval> &results)
      -> wh::core::result<wh::retriever::retriever_response> {
    wh::retriever::retriever_response fused{};
    for (const auto &result : results) {
      fused.insert(fused.end(), result.documents.begin(), result.documents.end());
    }
    return fused;
  };
  using rewriter_t = decltype(rewriter);
  using fusion_t = decltype(fusion);

  auto multi_query = wh::flow::retrieval::multi_query<retriever_t, rewriter_t, fusion_t>{
      make_retriever("mq:"), rewriter, fusion};
  REQUIRE(multi_query.set_max_queries(0U).has_value());
  REQUIRE(multi_query.freeze().has_value());

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(multi_query.retrieve(request, context));
  REQUIRE(waited.has_value());

  auto status = std::move(std::get<0>(waited.value()));
  REQUIRE(status.has_value());
  REQUIRE(status.value().size() == 5U);

  std::vector<std::string> contents{};
  for (const auto &document : status.value()) {
    contents.push_back(document.content());
  }
  REQUIRE(contents == std::vector<std::string>{"mq:hello", "mq:hello-alt", "mq:hello-2",
                                               "mq:hello-3", "mq:hello-4"});
}
