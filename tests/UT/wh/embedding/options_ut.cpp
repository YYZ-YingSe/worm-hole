#include <catch2/catch_test_macros.hpp>

#include "wh/embedding/options.hpp"

namespace {

struct embedding_options_probe {
  int value{0};
};

} // namespace

TEST_CASE("embedding options resolve base and override layers",
          "[UT][wh/embedding/options.hpp][embedding_options::resolve][branch][boundary]") {
  wh::embedding::embedding_options options{};
  wh::embedding::embedding_common_options base{};
  base.model_id = "base-model";
  base.failure_policy = wh::embedding::batch_failure_policy::partial_success;
  options.set_base(base);

  auto base_view = options.resolve_view();
  REQUIRE(base_view.model_id == "base-model");
  REQUIRE(base_view.failure_policy == wh::embedding::batch_failure_policy::partial_success);

  wh::embedding::embedding_common_options override{};
  override.model_id = "override-model";
  override.failure_policy = wh::embedding::batch_failure_policy::fail_fast;
  options.set_call_override(std::move(override));

  const auto view = options.resolve_view();
  REQUIRE(view.model_id == "override-model");
  REQUIRE(view.failure_policy == wh::embedding::batch_failure_policy::fail_fast);

  const auto resolved = options.resolve();
  REQUIRE(resolved.model_id == "override-model");
  REQUIRE(resolved.failure_policy == wh::embedding::batch_failure_policy::fail_fast);
}

TEST_CASE("embedding options keep component specific payload",
          "[UT][wh/embedding/options.hpp][embedding_options::component_options][boundary]") {
  wh::embedding::embedding_options options{};
  options.component_options().set_impl_specific(embedding_options_probe{9});
  const auto *probe = options.component_options().impl_specific_if<embedding_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 9);
}

TEST_CASE("embedding options keep base model when override model id is empty",
          "[UT][wh/embedding/options.hpp][embedding_options::resolve_view][condition][boundary]") {
  wh::embedding::embedding_options options{};
  options.set_base(wh::embedding::embedding_common_options{
      .model_id = "base-model",
      .failure_policy = wh::embedding::batch_failure_policy::partial_success,
  });
  options.set_call_override(wh::embedding::embedding_common_options{
      .model_id = "",
      .failure_policy = wh::embedding::batch_failure_policy::fail_fast,
  });

  const auto view = options.resolve_view();
  REQUIRE(view.model_id == "base-model");
  REQUIRE(view.failure_policy == wh::embedding::batch_failure_policy::fail_fast);
}
