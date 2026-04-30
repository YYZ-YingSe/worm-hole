#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/authored/branch.hpp"
#include "wh/core/stdexec/result_sender.hpp"

TEST_CASE("compose authored branch facade exposes both branch builders",
          "[UT][wh/compose/authored/branch.hpp][stream_branch][boundary]") {
  STATIC_REQUIRE(std::is_default_constructible_v<wh::compose::value_branch>);
  STATIC_REQUIRE(std::is_default_constructible_v<wh::compose::stream_branch>);

  wh::compose::value_branch value{};
  wh::compose::stream_branch stream{};
  REQUIRE(value.end_nodes().empty());
  REQUIRE(stream.end_nodes().empty());
}

TEST_CASE("value branch validates case descriptors and reports declared end nodes",
          "[UT][wh/compose/authored/branch.hpp][value_branch::add_case][condition][branch]") {
  wh::compose::value_branch branch{};

  auto invalid = branch.add_case(wh::compose::value_branch_case{});
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  auto added =
      branch.add_case("next",
                      [](const wh::compose::graph_value &,
                         wh::core::run_context &) -> wh::core::result<bool> { return true; });
  REQUIRE(added.has_value());
  REQUIRE(branch.end_nodes() == std::vector<std::string>{"next"});
  REQUIRE(branch.cases().size() == 1U);
}

TEST_CASE("stream branch validates targets and selector installation",
          "[UT][wh/compose/authored/branch.hpp][stream_branch::set_selector][condition][branch]") {
  wh::compose::stream_branch branch{};

  auto invalid_target = branch.add_target("");
  REQUIRE(invalid_target.has_error());
  REQUIRE(invalid_target.error() == wh::core::errc::invalid_argument);

  REQUIRE(branch.add_target("left").has_value());
  REQUIRE(branch.add_target("right").has_value());
  REQUIRE(branch.end_nodes() == std::vector<std::string>({"left", "right"}));

  auto invalid_selector = branch.set_selector(wh::compose::stream_branch_selector{nullptr});
  REQUIRE(invalid_selector.has_error());
  REQUIRE(invalid_selector.error() == wh::core::errc::invalid_argument);

  auto valid_selector = branch.set_selector(
      [](wh::compose::graph_stream_reader,
         wh::core::run_context &) -> wh::compose::stream_branch_key_sender {
        return wh::compose::stream_branch_key_sender{
            wh::core::detail::ready_sender(
                wh::core::result<std::vector<std::string>>{std::vector<std::string>{"left"}})};
      });
  REQUIRE(valid_selector.has_value());
  REQUIRE(static_cast<bool>(branch.selector()));
}
