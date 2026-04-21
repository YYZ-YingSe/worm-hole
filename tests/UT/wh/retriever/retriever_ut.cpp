#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/retriever/retriever.hpp"

namespace {

struct sync_retriever_impl {
  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request) const
      -> wh::retriever::detail::retriever_result {
    wh::schema::document keep{request.query + "-1"};
    keep.with_score(0.9).with_sub_index("sub").with_dsl("dsl");
    keep.set_metadata("lang", "zh");

    wh::schema::document drop{"drop"};
    drop.with_score(0.1).with_sub_index("other").with_dsl("other");
    drop.set_metadata("lang", "en");
    return wh::retriever::retriever_response{keep, drop};
  }
};

struct async_retriever_impl {
  [[nodiscard]] auto retrieve_sender(const wh::retriever::retriever_request &request) const {
    return stdexec::just(wh::core::result<wh::retriever::retriever_response>{
        wh::retriever::retriever_response{wh::schema::document{request.query + "-async"}}});
  }
};

} // namespace

TEST_CASE("retriever wrapper applies response policy filters on sync outputs",
          "[UT][wh/retriever/retriever.hpp][retriever::retrieve][branch][boundary]") {
  wh::retriever::retriever wrapped{sync_retriever_impl{}};

  wh::retriever::retriever_request request{};
  request.query = "q";
  request.sub_index = "sub";
  request.options.set_base(wh::retriever::retriever_common_options{
      .top_k = 4U,
      .score_threshold = 0.5,
      .filter = "lang=zh",
      .dsl = "dsl",
      .merge_policy = wh::retriever::recall_merge_policy::dedupe_by_content,
  });

  wh::core::run_context context{};
  auto result = wrapped.retrieve(request, context);
  REQUIRE(result.has_value());
  REQUIRE(result.value().size() == 1U);
  REQUIRE(result.value().front().content() == "q-1");
}

TEST_CASE("retriever wrapper normalizes async sender outputs",
          "[UT][wh/retriever/retriever.hpp][retriever::async_retrieve][branch]") {
  wh::retriever::retriever wrapped{async_retriever_impl{}};
  wh::retriever::retriever_request request{};
  request.query = "q";
  wh::core::run_context context{};

  auto awaited = stdexec::sync_wait(wrapped.async_retrieve(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().front().content() == "q-async");
}

TEST_CASE("retriever detail filters dedupes truncates and exposes callback state",
          "[UT][wh/retriever/"
          "retriever.hpp][detail::apply_response_policy][condition][branch][boundary]") {
  wh::schema::document first{"same"};
  first.with_score(0.9).with_sub_index("sub").with_dsl("dsl");
  first.set_metadata("lang", "zh");

  wh::schema::document duplicate{"same"};
  duplicate.with_score(0.8).with_sub_index("sub").with_dsl("dsl");
  duplicate.set_metadata("lang", "zh");

  wh::schema::document second{"other"};
  second.with_score(0.7).with_sub_index("sub").with_dsl("dsl");
  second.set_metadata("lang", "en");

  REQUIRE(wh::retriever::detail::matches_filter_expression(first, "lang=zh"));
  REQUIRE(wh::retriever::detail::matches_filter_expression(first, "content=same"));
  REQUIRE_FALSE(wh::retriever::detail::matches_filter_expression(first, "lang=en"));

  wh::retriever::retriever_request request{};
  request.query = "search";
  request.sub_index = "sub";
  request.options.set_base(wh::retriever::retriever_common_options{
      .top_k = 1U,
      .score_threshold = 0.6,
      .filter = "lang=zh",
      .dsl = "dsl",
      .merge_policy = wh::retriever::recall_merge_policy::dedupe_by_content,
  });

  const auto state = wh::retriever::detail::make_callback_state(request);
  REQUIRE(state.event.top_k == 1U);
  REQUIRE(state.event.score_threshold == 0.6);

  auto applied = wh::retriever::detail::apply_response_policy(
      wh::retriever::retriever_response{first, duplicate, second},
      wh::retriever::detail::make_response_policy(request));
  REQUIRE(applied.has_value());
  REQUIRE(applied.value().size() == 1U);
  REQUIRE(applied.value().front().content() == "same");
}
