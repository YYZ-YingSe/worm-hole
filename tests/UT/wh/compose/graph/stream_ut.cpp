#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/stream.hpp"

namespace {

[[nodiscard]] auto extract_int(const wh::compose::graph_value &value) -> int {
  const auto *typed = wh::core::any_cast<int>(&value);
  REQUIRE(typed != nullptr);
  return *typed;
}

[[nodiscard]] auto extract_string(const wh::compose::graph_value &value) -> std::string {
  const auto *typed = wh::core::any_cast<std::string>(&value);
  REQUIRE(typed != nullptr);
  return *typed;
}

} // namespace

TEST_CASE("make_graph_stream creates connected writer reader endpoints",
          "[UT][wh/compose/graph/stream.hpp][make_graph_stream][branch][boundary]") {
  auto [writer, reader] = wh::compose::make_graph_stream(2U);
  REQUIRE(writer.try_write(wh::compose::graph_value{1}).has_value());
  REQUIRE(writer.try_write(wh::compose::graph_value{std::string{"two"}}).has_value());
  REQUIRE(writer.close().has_value());

  auto collected = wh::compose::collect_graph_stream_reader(std::move(reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 2U);
  REQUIRE(extract_int(collected.value()[0]) == 1);
  REQUIRE(extract_string(collected.value()[1]) == "two");
}

TEST_CASE(
    "copy_graph_readers duplicates source readers and handles zero fanout",
    "[UT][wh/compose/graph/stream.hpp][detail::copy_graph_readers][condition][branch][boundary]") {
  auto empty = wh::compose::detail::copy_graph_readers(wh::compose::graph_stream_reader{}, 0U);
  REQUIRE(empty.has_value());
  REQUIRE(empty.value().empty());

  auto source = wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
      wh::compose::graph_value{3}, wh::compose::graph_value{5}});
  REQUIRE(source.has_value());

  auto copies = wh::compose::detail::copy_graph_readers(std::move(source).value(), 2U);
  REQUIRE(copies.has_value());
  REQUIRE(copies.value().size() == 2U);

  auto first = wh::compose::collect_graph_stream_reader(std::move(copies.value()[0]));
  REQUIRE(first.has_value());
  REQUIRE(first.value().size() == 2U);
  REQUIRE(extract_int(first.value()[0]) == 3);
  REQUIRE(extract_int(first.value()[1]) == 5);

  auto second = wh::compose::collect_graph_stream_reader(std::move(copies.value()[1]));
  REQUIRE(second.has_value());
  REQUIRE(second.value().size() == 2U);
  REQUIRE(extract_int(second.value()[0]) == 3);
  REQUIRE(extract_int(second.value()[1]) == 5);
}

TEST_CASE("make_graph_merge_reader merges named readers and builds dynamic source shells",
          "[UT][wh/compose/graph/"
          "stream.hpp][detail::make_graph_merge_reader][condition][branch][boundary]") {
  auto left = wh::compose::make_single_value_stream_reader(1);
  REQUIRE(left.has_value());
  auto right = wh::compose::make_single_value_stream_reader(2);
  REQUIRE(right.has_value());

  std::vector<wh::schema::stream::named_stream_reader<wh::compose::graph_stream_reader>> named{};
  named.emplace_back("left", std::move(left).value());
  named.emplace_back("right", std::move(right).value());

  auto merged = wh::compose::detail::make_graph_merge_reader(std::move(named));
  auto collected = wh::compose::collect_graph_stream_reader(std::move(merged));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 2U);

  std::vector<int> merged_values{};
  merged_values.push_back(extract_int(collected.value()[0]));
  merged_values.push_back(extract_int(collected.value()[1]));
  std::ranges::sort(merged_values);
  const std::vector<int> expected_values{1, 2};
  REQUIRE(merged_values == expected_values);

  auto topology_shell =
      wh::compose::detail::make_graph_merge_reader(std::vector<std::string>{"left", "right"});
  REQUIRE(topology_shell.close().has_value());
}

TEST_CASE("fork_graph_reader_payload duplicates stream payloads and rejects non-reader values",
          "[UT][wh/compose/graph/"
          "stream.hpp][detail::fork_graph_reader_payload][condition][branch][boundary]") {
  wh::compose::graph_value invalid{9};
  auto invalid_status = wh::compose::detail::fork_graph_reader_payload(invalid);
  REQUIRE(invalid_status.has_error());
  REQUIRE(invalid_status.error() == wh::core::errc::type_mismatch);

  auto reader = wh::compose::make_values_stream_reader(
      std::vector<wh::compose::graph_value>{wh::compose::graph_value{7}});
  REQUIRE(reader.has_value());
  wh::compose::graph_value payload{std::move(reader).value()};

  auto sibling = wh::compose::detail::fork_graph_reader_payload(payload);
  REQUIRE(sibling.has_value());

  auto *first_reader = wh::core::any_cast<wh::compose::graph_stream_reader>(&payload);
  REQUIRE(first_reader != nullptr);
  auto first_collected = wh::compose::collect_graph_stream_reader(std::move(*first_reader));
  REQUIRE(first_collected.has_value());
  REQUIRE(first_collected.value().size() == 1U);
  REQUIRE(extract_int(first_collected.value()[0]) == 7);

  auto *second_reader = wh::core::any_cast<wh::compose::graph_stream_reader>(&sibling.value());
  REQUIRE(second_reader != nullptr);
  auto second_collected = wh::compose::collect_graph_stream_reader(std::move(*second_reader));
  REQUIRE(second_collected.has_value());
  REQUIRE(second_collected.value().size() == 1U);
  REQUIRE(extract_int(second_collected.value()[0]) == 7);
}

TEST_CASE("value payload validators reject reader and move-only payload violations",
          "[UT][wh/compose/graph/"
          "stream.hpp][detail::validate_value_contract_payload][condition][branch][boundary]") {
  wh::compose::graph_value copyable{std::string{"value"}};
  REQUIRE(wh::compose::detail::is_reader_value_payload(copyable) == false);
  REQUIRE(wh::compose::detail::validate_value_boundary_payload(copyable).has_value());
  REQUIRE(wh::compose::detail::validate_copyable_value_payload(copyable).has_value());
  REQUIRE(wh::compose::detail::validate_value_contract_payload(copyable).has_value());

  auto reader = wh::compose::make_single_value_stream_reader(4);
  REQUIRE(reader.has_value());
  wh::compose::graph_value reader_payload{std::move(reader).value()};
  REQUIRE(wh::compose::detail::is_reader_value_payload(reader_payload));
  auto boundary = wh::compose::detail::validate_value_boundary_payload(reader_payload);
  REQUIRE(boundary.has_error());
  REQUIRE(boundary.error() == wh::core::errc::contract_violation);
  auto contract = wh::compose::detail::validate_value_contract_payload(reader_payload);
  REQUIRE(contract.has_error());
  REQUIRE(contract.error() == wh::core::errc::contract_violation);

  wh::compose::graph_value move_only{std::make_unique<int>(9)};
  auto copyable_status = wh::compose::detail::validate_copyable_value_payload(move_only);
  REQUIRE(copyable_status.has_error());
  REQUIRE(copyable_status.error() == wh::core::errc::contract_violation);
}

TEST_CASE(
    "to_graph_stream_reader accepts graph readers and converts typed readers",
    "[UT][wh/compose/graph/stream.hpp][to_graph_stream_reader][condition][branch][boundary]") {
  STATIC_REQUIRE(
      wh::compose::graph_stream_status<wh::core::result<wh::compose::graph_stream_reader>>);
  STATIC_REQUIRE_FALSE(wh::compose::graph_stream_status<wh::core::result<int>>);

  auto identity = wh::compose::make_single_value_stream_reader(6);
  REQUIRE(identity.has_value());
  auto canonical_identity = wh::compose::to_graph_stream_reader(std::move(identity).value());
  REQUIRE(canonical_identity.has_value());
  auto identity_values =
      wh::compose::collect_graph_stream_reader(std::move(canonical_identity).value());
  REQUIRE(identity_values.has_value());
  REQUIRE(identity_values.value().size() == 1U);
  REQUIRE(extract_int(identity_values.value()[0]) == 6);

  auto graph_reader = wh::schema::stream::make_values_stream_reader(
      std::vector<wh::compose::graph_value>{wh::compose::graph_value{std::string{"graph"}}});
  auto canonical_graph = wh::compose::to_graph_stream_reader(std::move(graph_reader));
  REQUIRE(canonical_graph.has_value());
  auto graph_values = wh::compose::collect_graph_stream_reader(std::move(canonical_graph).value());
  REQUIRE(graph_values.has_value());
  REQUIRE(graph_values.value().size() == 1U);
  REQUIRE(extract_string(graph_values.value()[0]) == "graph");

  auto typed_reader = wh::schema::stream::make_values_stream_reader(std::vector<int>{8, 9});
  auto canonical_typed = wh::compose::to_graph_stream_reader(std::move(typed_reader));
  REQUIRE(canonical_typed.has_value());
  auto typed_values = wh::compose::collect_graph_stream_reader(std::move(canonical_typed).value());
  REQUIRE(typed_values.has_value());
  REQUIRE(typed_values.value().size() == 2U);
  REQUIRE(extract_int(typed_values.value()[0]) == 8);
  REQUIRE(extract_int(typed_values.value()[1]) == 9);
}

TEST_CASE("make_values_stream_reader make_single_value_stream_reader and fork_graph_value preserve "
          "copyable values",
          "[UT][wh/compose/graph/stream.hpp][fork_graph_value][condition][branch][boundary]") {
  const std::vector<wh::compose::graph_value> copied_values{
      wh::compose::graph_value{1},
      wh::compose::graph_value{std::string{"two"}},
  };
  auto copied_reader = wh::compose::make_values_stream_reader(copied_values);
  REQUIRE(copied_reader.has_value());
  auto copied_collected =
      wh::compose::collect_graph_stream_reader(std::move(copied_reader).value());
  REQUIRE(copied_collected.has_value());
  REQUIRE(copied_collected.value().size() == 2U);
  REQUIRE(extract_int(copied_collected.value()[0]) == 1);
  REQUIRE(extract_string(copied_collected.value()[1]) == "two");

  auto single = wh::compose::make_single_value_stream_reader(7);
  REQUIRE(single.has_value());
  auto single_collected = wh::compose::collect_graph_stream_reader(std::move(single).value());
  REQUIRE(single_collected.has_value());
  REQUIRE(single_collected.value().size() == 1U);
  REQUIRE(extract_int(single_collected.value()[0]) == 7);

  wh::compose::graph_value value{std::string{"x"}};
  auto forked = wh::compose::fork_graph_value(value);
  REQUIRE(forked.has_value());
  REQUIRE(extract_string(forked.value()) == "x");

  std::string borrowed_source = "seed";
  wh::compose::graph_value borrowed_value{wh::core::any::ref(borrowed_source)};
  auto borrowed_fork = wh::compose::fork_graph_value(borrowed_value);
  REQUIRE(borrowed_fork.has_value());
  REQUIRE(extract_string(borrowed_fork.value()) == "seed");
  borrowed_source = "mutated";
  REQUIRE(extract_string(borrowed_fork.value()) == "seed");

  auto reader_payload = wh::compose::make_single_value_stream_reader(1);
  REQUIRE(reader_payload.has_value());
  wh::compose::graph_value invalid_reader{std::move(reader_payload).value()};
  auto reader_fork = wh::compose::fork_graph_value(invalid_reader);
  REQUIRE(reader_fork.has_error());
  REQUIRE(reader_fork.error() == wh::core::errc::contract_violation);

  wh::compose::graph_value move_only{std::make_unique<int>(4)};
  auto move_only_fork = wh::compose::fork_graph_value(move_only);
  REQUIRE(move_only_fork.has_error());
  REQUIRE(move_only_fork.error() == wh::core::errc::not_supported);
}
