#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/collect_policy.hpp"

TEST_CASE("collect policy drains value chunks until terminal eof and returns collected vector",
          "[UT][wh/compose/graph/detail/"
          "collect_policy.hpp][collect_policy::handle_completion][condition][branch][boundary]") {
  std::vector<wh::compose::graph_value> values{};
  values.emplace_back(1);
  values.emplace_back(2);
  auto reader = wh::compose::make_values_stream_reader(values);
  REQUIRE(reader.has_value());

  wh::compose::detail::collect_policy policy{
      .reader = std::move(reader).value(),
      .limits = wh::compose::edge_limits{.max_items = 2U},
  };
  policy.start();
  REQUIRE(policy.collected.capacity() >= 2U);

  auto step = policy.next_step();
  REQUIRE(step.has_value());
  REQUIRE(step->sender.has_value());

  auto first = policy.reader.read();
  REQUIRE(first.has_value());
  auto first_status = policy.handle_completion(std::move(first).value());
  REQUIRE_FALSE(first_status.has_value());
  REQUIRE(policy.collected.size() == 1U);

  auto second = policy.reader.read();
  REQUIRE(second.has_value());
  auto second_status = policy.handle_completion(std::move(second).value());
  REQUIRE_FALSE(second_status.has_value());
  REQUIRE(policy.collected.size() == 2U);

  auto eof = policy.reader.read();
  REQUIRE(eof.has_value());
  auto finished = policy.handle_completion(std::move(eof).value());
  REQUIRE(finished.has_value());
  REQUIRE(finished->has_value());
  auto *collected = wh::core::any_cast<std::vector<wh::compose::graph_value>>(&finished->value());
  REQUIRE(collected != nullptr);
  REQUIRE(collected->size() == 2U);
  REQUIRE(*wh::core::any_cast<int>(&(*collected)[0]) == 1);
  REQUIRE(*wh::core::any_cast<int>(&(*collected)[1]) == 2);
}

TEST_CASE(
    "collect policy enforces item limits and propagates read errors",
    "[UT][wh/compose/graph/detail/collect_policy.hpp][collect_policy::next_step][branch][error]") {
  std::vector<wh::compose::graph_value> values{};
  values.emplace_back(1);
  values.emplace_back(2);
  auto reader = wh::compose::make_values_stream_reader(values);
  REQUIRE(reader.has_value());

  wh::compose::detail::collect_policy limited{
      .reader = std::move(reader).value(),
      .limits = wh::compose::edge_limits{.max_items = 1U},
  };
  auto first = limited.reader.read();
  REQUIRE(first.has_value());
  REQUIRE_FALSE(limited.handle_completion(std::move(first).value()).has_value());

  auto second = limited.reader.read();
  REQUIRE(second.has_value());
  auto overflow = limited.handle_completion(std::move(second).value());
  REQUIRE(overflow.has_value());
  REQUIRE(overflow->has_error());
  REQUIRE(overflow->error() == wh::core::errc::resource_exhausted);

  wh::compose::detail::collect_policy error_policy{};
  auto errored =
      error_policy.handle_completion(wh::compose::graph_stream_reader::chunk_result_type::failure(
          wh::core::errc::invalid_argument));
  REQUIRE(errored.has_value());
  REQUIRE(errored->has_error());
  REQUIRE(errored->error() == wh::core::errc::invalid_argument);
}
