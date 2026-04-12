#include <catch2/catch_test_macros.hpp>

#include "wh/indexer/options.hpp"

namespace {

struct indexer_options_probe {
  int value{0};
};

} // namespace

TEST_CASE("indexer options merge base and override fields with string fallbacks",
          "[UT][wh/indexer/options.hpp][indexer_options::resolve][branch][boundary]") {
  wh::indexer::indexer_options options{};
  wh::indexer::indexer_common_options base{};
  base.failure_policy = wh::indexer::write_failure_policy::retry;
  base.max_retries = 2U;
  base.sub_index = "base-sub";
  base.embedding_model = "embed-a";
  base.combine_with_embedding = false;
  options.set_base(base);

  wh::indexer::indexer_common_options override{};
  override.failure_policy = wh::indexer::write_failure_policy::skip;
  override.max_retries = 5U;
  override.sub_index = "override-sub";
  override.combine_with_embedding = true;
  options.set_call_override(std::move(override));

  const auto view = options.resolve_view();
  REQUIRE(view.failure_policy == wh::indexer::write_failure_policy::skip);
  REQUIRE(view.max_retries == 5U);
  REQUIRE(view.sub_index == "override-sub");
  REQUIRE(view.embedding_model == "embed-a");
  REQUIRE(view.combine_with_embedding);

  const auto resolved = options.resolve();
  REQUIRE(resolved.failure_policy == wh::indexer::write_failure_policy::skip);
  REQUIRE(resolved.sub_index == "override-sub");
  REQUIRE(resolved.embedding_model == "embed-a");
  REQUIRE(resolved.combine_with_embedding);
}

TEST_CASE("indexer options expose component specific extras",
          "[UT][wh/indexer/options.hpp][indexer_options::component_options][boundary]") {
  wh::indexer::indexer_options options{};
  options.component_options().set_impl_specific(indexer_options_probe{5});
  const auto *probe =
      options.component_options().impl_specific_if<indexer_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 5);
}

TEST_CASE("indexer options preserve base strings when override leaves them empty",
          "[UT][wh/indexer/options.hpp][indexer_options::resolve_view][condition][boundary]") {
  wh::indexer::indexer_options options{};
  options.set_base(wh::indexer::indexer_common_options{
      .failure_policy = wh::indexer::write_failure_policy::retry,
      .max_retries = 2U,
      .sub_index = "base-sub",
      .embedding_model = "embed-a",
      .combine_with_embedding = false,
  });
  options.set_call_override(wh::indexer::indexer_common_options{
      .failure_policy = wh::indexer::write_failure_policy::skip,
      .max_retries = 1U,
      .sub_index = "",
      .embedding_model = "",
      .combine_with_embedding = true,
  });

  const auto view = options.resolve_view();
  REQUIRE(view.failure_policy == wh::indexer::write_failure_policy::skip);
  REQUIRE(view.max_retries == 1U);
  REQUIRE(view.sub_index == "base-sub");
  REQUIRE(view.embedding_model == "embed-a");
  REQUIRE(view.combine_with_embedding);
}
