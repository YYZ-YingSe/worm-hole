#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <tuple>
#include <type_traits>
#include <unordered_set>

#include <stdexec/execution.hpp>

#include "wh/flow/retrieval/router.hpp"

namespace {

template <typename text_t>
[[nodiscard]] auto make_document(text_t &&text, double score = 0.0)
    -> wh::schema::document {
  wh::schema::document document{std::forward<text_t>(text)};
  document.with_score(score);
  return document;
}

struct routed_retriever_impl {
  std::string prefix{};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {prefix, wh::core::component_kind::retriever};
  }

  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request) const
      -> wh::core::result<wh::retriever::retriever_response> {
    return wh::retriever::retriever_response{
        make_document(prefix + ":" + request.query, prefix == "left" ? 1.0 : 2.0)};
  }
};

} // namespace

TEST_CASE("retrieval router exposes helper keys and fuses ranked results",
          "[UT][wh/flow/retrieval/router.hpp][reciprocal_rank_fusion][branch][boundary]") {
  REQUIRE(wh::flow::retrieval::detail::router::router_stage_name == "Router");
  REQUIRE(wh::flow::retrieval::detail::router::fusion_stage_name == "FusionFunc");

  wh::flow::retrieval::detail::router::reciprocal_rank_fusion fusion{};
  auto fused = fusion({
      {.retriever_name = "a",
       .documents = {make_document("dup", 0.1), make_document("x", 0.2)}},
      {.retriever_name = "b",
       .documents = {make_document("dup", 0.9), make_document("y", 0.3)}},
  });
  REQUIRE(fused.has_value());
  REQUIRE(fused.value().size() == 3U);
  REQUIRE(fused.value().front().content() == "dup");
}

TEST_CASE("retrieval router validates registration and executes selected routes",
          "[UT][wh/flow/retrieval/router.hpp][router::add_retriever][branch][boundary]") {
  using retriever_t = wh::retriever::retriever<routed_retriever_impl>;

  wh::flow::retrieval::router<retriever_t> router{};
  STATIC_REQUIRE(std::is_copy_constructible_v<decltype(router)>);
  REQUIRE(router.add_retriever("", retriever_t{routed_retriever_impl{"bad"}}).has_error());
  REQUIRE(router.add_retriever("left", retriever_t{routed_retriever_impl{"left"}})
              .has_value());
  REQUIRE(router.add_retriever("right", retriever_t{routed_retriever_impl{"right"}})
              .has_value());
  REQUIRE(router.add_retriever("left", retriever_t{routed_retriever_impl{"dup"}})
              .has_error());

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(router.retrieve(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().size() == 2U);
  std::unordered_set<std::string> contents{};
  for (const auto &document : std::get<0>(*awaited).value()) {
    contents.insert(document.content());
  }
  REQUIRE(contents.contains("left:hello"));
  REQUIRE(contents.contains("right:hello"));
  REQUIRE(router.frozen());
  REQUIRE(router.add_retriever("late", retriever_t{routed_retriever_impl{"late"}})
              .has_error());
}

TEST_CASE("retrieval router succeeds when route policy selects one registered subset",
          "[UT][wh/flow/retrieval/router.hpp][router::retrieve][happy][subset]") {
  using retriever_t = wh::retriever::retriever<routed_retriever_impl>;

  auto select_left = [](const wh::retriever::retriever_request &,
                        const std::vector<std::string> &)
      -> wh::core::result<std::vector<std::string>> {
    return std::vector<std::string>{"left"};
  };
  wh::flow::retrieval::router<retriever_t, decltype(select_left)> router{
      select_left};
  REQUIRE(router.add_retriever("left",
                               retriever_t{routed_retriever_impl{"left"}})
              .has_value());
  REQUIRE(router.add_retriever("right",
                               retriever_t{routed_retriever_impl{"right"}})
              .has_value());

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(router.retrieve(request, context));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(std::get<0>(*waited).value().size() == 1U);
  REQUIRE(std::get<0>(*waited).value().front().content() == "left:hello");
}

TEST_CASE("retrieval router propagates route and fusion failures",
          "[UT][wh/flow/retrieval/router.hpp][router::retrieve][branch]") {
  using retriever_t = wh::retriever::retriever<routed_retriever_impl>;

  auto missing_route = [](const wh::retriever::retriever_request &,
                          const std::vector<std::string> &)
      -> wh::core::result<std::vector<std::string>> {
    return std::vector<std::string>{"missing"};
  };
  wh::flow::retrieval::router<retriever_t, decltype(missing_route)> missing_router{
      missing_route};
  REQUIRE(missing_router.add_retriever("left",
                                       retriever_t{routed_retriever_impl{"left"}})
              .has_value());

  wh::retriever::retriever_request request{};
  request.query = "hello";
  wh::core::run_context context{};
  auto missing_waited = stdexec::sync_wait(missing_router.retrieve(request, context));
  REQUIRE(missing_waited.has_value());
  REQUIRE(std::get<0>(*missing_waited).has_error());
  REQUIRE(std::get<0>(*missing_waited).error() == wh::core::errc::not_found);

  auto route_all = wh::flow::retrieval::detail::router::route_all_retrievers{};
  auto failing_fusion = [](const std::vector<wh::flow::retrieval::routed_retriever_result> &)
      -> wh::core::result<wh::retriever::retriever_response> {
    return wh::core::result<wh::retriever::retriever_response>::failure(
        wh::core::errc::network_error);
  };
  wh::flow::retrieval::router<retriever_t, decltype(route_all), decltype(failing_fusion)>
      failing_router{route_all, failing_fusion};
  REQUIRE(failing_router.add_retriever(
              "left", retriever_t{routed_retriever_impl{"left"}})
              .has_value());
  auto failing_waited = stdexec::sync_wait(failing_router.retrieve(request, context));
  REQUIRE(failing_waited.has_value());
  REQUIRE(std::get<0>(*failing_waited).has_error());
  REQUIRE(std::get<0>(*failing_waited).error() == wh::core::errc::network_error);
}

TEST_CASE("retrieval router default route policy selects every registered retriever once",
          "[UT][wh/flow/retrieval/router.hpp][route_all_retrievers][condition][boundary]") {
  const std::vector<std::string> names{"left", "right", "left"};
  wh::retriever::retriever_request request{};
  const auto routed =
      wh::flow::retrieval::detail::router::route_all_retrievers{}(request, names);

  REQUIRE(routed.has_value());
  REQUIRE(routed.value() == names);
}
