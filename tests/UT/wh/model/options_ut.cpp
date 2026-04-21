#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/model/options.hpp"

namespace {

struct model_options_probe {
  int value{0};
};

} // namespace

TEST_CASE("chat model options resolve base and override layers across lists and policies",
          "[UT][wh/model/options.hpp][chat_model_options::resolve][branch][boundary]") {
  wh::model::chat_model_options options{};
  wh::model::chat_model_common_options base{};
  base.model_id = "base";
  base.temperature = 0.1;
  base.top_p = 0.2;
  base.max_tokens = 256U;
  base.stop_tokens = {"A", "B"};
  base.selection_policy = wh::model::model_selection_policy::quality_first;
  base.fallback.ordered_candidates = {"c1"};
  base.tool_choice.mode = wh::schema::tool_call_mode::allow;
  base.allowed_tool_names = {"t1"};
  base.structured_output.preference =
      wh::model::structured_output_preference::provider_native_first;
  options.set_base(base);

  wh::model::chat_model_common_options override{};
  override.model_id = "override";
  override.temperature = 0.7;
  override.top_p = 0.9;
  override.max_tokens = 512U;
  override.selection_policy = wh::model::model_selection_policy::cost_first;
  override.tool_choice.mode = wh::schema::tool_call_mode::force;
  override.allowed_tool_names = {"t2", "t3"};
  override.structured_output.preference = wh::model::structured_output_preference::tool_call_first;
  options.set_call_override(std::move(override));

  const auto view = options.resolve_view();
  REQUIRE(view.model_id == "override");
  REQUIRE(view.temperature == 0.7);
  REQUIRE(view.top_p == 0.9);
  REQUIRE(view.max_tokens == 512U);
  REQUIRE(view.stop_tokens.size() == 2U);
  REQUIRE(view.stop_tokens[0] == "A");
  REQUIRE(view.selection_policy == wh::model::model_selection_policy::cost_first);
  REQUIRE(view.tool_choice_ref().mode == wh::schema::tool_call_mode::force);
  REQUIRE(view.allowed_tool_names.size() == 2U);
  REQUIRE(view.structured_output_ref().preference ==
          wh::model::structured_output_preference::tool_call_first);

  const auto resolved = options.resolve();
  REQUIRE(resolved.model_id == "override");
  REQUIRE(resolved.stop_tokens == std::vector<std::string>{"A", "B"});
  REQUIRE(resolved.allowed_tool_names == std::vector<std::string>{"t2", "t3"});
}

TEST_CASE("chat model options freeze candidate order and negotiate structured output",
          "[UT][wh/model/options.hpp][freeze_model_candidates][branch]") {
  wh::model::chat_model_common_options options{};
  options.model_id = "primary";
  options.fallback.ordered_candidates = {"backup", "primary", "extra"};

  const auto quality = wh::model::freeze_model_candidates(
      options, std::span<const std::string>{std::array<std::string, 2>{"disc", "backup"}});
  REQUIRE(quality == std::vector<std::string>{"primary", "disc", "backup", "extra"});

  options.selection_policy = wh::model::model_selection_policy::cost_first;
  const auto cost = wh::model::freeze_model_candidates(
      options, std::span<const std::string>{std::array<std::string, 1>{"disc"}});
  REQUIRE(cost == std::vector<std::string>{"extra", "backup", "disc", "primary"});

  options.selection_policy = wh::model::model_selection_policy::latency_first;
  const auto latency = wh::model::freeze_model_candidates(
      options, std::span<const std::string>{std::array<std::string, 1>{"disc"}});
  REQUIRE(latency == std::vector<std::string>{"disc", "backup", "extra", "primary"});

  const auto provider_first = wh::model::negotiate_structured_output(
      wh::model::structured_output_policy{
          .preference = wh::model::structured_output_preference::provider_native_first,
          .allow_tool_fallback = true,
      },
      false, true);
  REQUIRE_FALSE(provider_first.use_provider_native);
  REQUIRE(provider_first.use_tool_call_fallback);

  const auto tool_first = wh::model::negotiate_structured_output(
      wh::model::structured_output_policy{
          .preference = wh::model::structured_output_preference::tool_call_first,
          .allow_tool_fallback = false,
      },
      true, false);
  REQUIRE(tool_first.use_provider_native);
  REQUIRE_FALSE(tool_first.use_tool_call_fallback);
}

TEST_CASE("chat model options expose component specific extras",
          "[UT][wh/model/options.hpp][chat_model_options::component_options][boundary]") {
  wh::model::chat_model_options options{};
  options.component_options().set_impl_specific(model_options_probe{13});
  const auto *probe = options.component_options().impl_specific_if<model_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 13);
}

TEST_CASE("chat model options preserve base collections when override leaves them empty",
          "[UT][wh/model/options.hpp][chat_model_options::resolve_view][condition][boundary]") {
  wh::model::chat_model_options options{};
  wh::model::chat_model_common_options base{};
  base.model_id = "base";
  base.stop_tokens = {"A", "B"};
  base.allowed_tool_names = {"t1"};
  base.fallback.ordered_candidates = {"backup"};
  options.set_base(base);

  wh::model::chat_model_common_options override{};
  override.temperature = 0.9;
  override.top_p = 0.8;
  override.max_tokens = 7U;
  options.set_call_override(std::move(override));

  const auto view = options.resolve_view();
  REQUIRE(view.model_id == "base");
  REQUIRE(view.stop_tokens.size() == 2U);
  REQUIRE(view.allowed_tool_names.size() == 1U);
  REQUIRE(view.fallback_ref().ordered_candidates == std::vector<std::string>{"backup"});
}
