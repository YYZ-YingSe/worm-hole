#include <catch2/catch_test_macros.hpp>

#include "wh/retriever/options.hpp"

namespace {

struct retriever_options_probe {
  int value{0};
};

} // namespace

TEST_CASE("retriever options resolve overrides and preserve base strings when empty",
          "[UT][wh/retriever/options.hpp][retriever_options::resolve][branch][boundary]") {
  wh::retriever::retriever_options options{};
  wh::retriever::retriever_common_options base{};
  base.top_k = 6U;
  base.score_threshold = 0.4;
  base.filter = "lang=zh";
  base.dsl = "faq";
  base.merge_policy = wh::retriever::recall_merge_policy::dedupe_by_content;
  base.fail_fast_on_route_error = true;
  options.set_base(base);

  wh::retriever::retriever_common_options override{};
  override.top_k = 3U;
  override.score_threshold = 0.8;
  override.merge_policy = wh::retriever::recall_merge_policy::concat;
  override.fail_fast_on_route_error = false;
  options.set_call_override(std::move(override));

  const auto view = options.resolve_view();
  REQUIRE(view.top_k == 3U);
  REQUIRE(view.score_threshold == 0.8);
  REQUIRE(view.filter == "lang=zh");
  REQUIRE(view.dsl == "faq");
  REQUIRE(view.merge_policy == wh::retriever::recall_merge_policy::concat);
  REQUIRE_FALSE(view.fail_fast_on_route_error);

  const auto resolved = options.resolve();
  REQUIRE(resolved.filter == "lang=zh");
  REQUIRE(resolved.dsl == "faq");
  REQUIRE(resolved.top_k == 3U);
  REQUIRE_FALSE(resolved.fail_fast_on_route_error);
}

TEST_CASE("retriever options expose component specific extras",
          "[UT][wh/retriever/options.hpp][retriever_options::component_options]") {
  wh::retriever::retriever_options options{};
  options.component_options().set_impl_specific(retriever_options_probe{11});
  const auto *probe =
      options.component_options().impl_specific_if<retriever_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 11);
}

TEST_CASE(
    "retriever options keep base values without override and expose direct impl-specific lookup",
    "[UT][wh/retriever/options.hpp][retriever_options::set_impl_specific][condition][branch][boundary]") {
  wh::retriever::retriever_options options{};
  options.set_base(wh::retriever::retriever_common_options{
      .top_k = 8U,
      .score_threshold = 0.25,
      .filter = "kind=faq",
      .dsl = "support",
      .merge_policy = wh::retriever::recall_merge_policy::dedupe_by_content,
      .fail_fast_on_route_error = true});
  options.set_impl_specific(retriever_options_probe{29});

  const auto view = options.resolve_view();
  REQUIRE(view.top_k == 8U);
  REQUIRE(view.score_threshold == 0.25);
  REQUIRE(view.filter == "kind=faq");
  REQUIRE(view.dsl == "support");
  REQUIRE(view.merge_policy ==
          wh::retriever::recall_merge_policy::dedupe_by_content);
  REQUIRE(view.fail_fast_on_route_error);

  const auto *probe = options.impl_specific_if<retriever_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 29);
}
