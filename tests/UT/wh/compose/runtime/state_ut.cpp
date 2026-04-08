#include <catch2/catch_test_macros.hpp>

#include "wh/compose/runtime/state.hpp"

TEST_CASE("graph process state supports typed storage lookup and parent fallback",
          "[UT][wh/compose/runtime/state.hpp][graph_process_state::get][condition][branch][boundary]") {
  wh::compose::graph_process_state parent{};
  REQUIRE(parent.emplace<int>(7).has_value());

  wh::compose::graph_process_state child{&parent};
  auto inherited = child.get<int>();
  REQUIRE(inherited.has_value());
  REQUIRE(inherited.value().get() == 7);

  REQUIRE(child.emplace<std::string>("local").has_value());
  auto local = child.get<std::string>();
  REQUIRE(local.has_value());
  REQUIRE(local.value().get() == "local");

  const auto &const_child = child;
  auto const_local = const_child.get<std::string>();
  REQUIRE(const_local.has_value());
  REQUIRE(const_local.value().get() == "local");
  REQUIRE(child.parent() == &parent);
}

TEST_CASE("graph process state reports not found and type mismatch details",
          "[UT][wh/compose/runtime/state.hpp][graph_process_state::last_error_detail][condition][branch][boundary]") {
  wh::compose::graph_process_state state{};
  REQUIRE(state.emplace<int>(1).has_value());

  auto mismatch = state.get<double>();
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::not_found);
  REQUIRE(wh::compose::graph_process_state::last_error_detail().find("state not found") !=
          std::string_view::npos);
  REQUIRE(wh::compose::detail::state_type_name<int>().empty() == false);
  REQUIRE(wh::compose::detail::state_not_found_detail<int>().find("expected=") !=
          std::string::npos);
  REQUIRE(wh::compose::detail::state_type_mismatch_detail<int>(
              wh::core::any{std::string{"bad"}})
              .find("actual=") != std::string::npos);
}

TEST_CASE("graph state table tracks keyed lifecycle updates and snapshots",
          "[UT][wh/compose/runtime/state.hpp][graph_state_table::update][condition][branch][boundary]") {
  std::vector<std::string> keys{"a", "b"};
  wh::compose::graph_state_table table{};
  table.reset(keys);

  auto by_missing = table.by_key("missing");
  REQUIRE(by_missing.has_error());
  REQUIRE(by_missing.error() == wh::core::errc::not_found);

  auto by_id_missing = table.by_id(5U);
  REQUIRE(by_id_missing.has_error());
  REQUIRE(by_id_missing.error() == wh::core::errc::not_found);

  REQUIRE(table
              .update("a", wh::compose::graph_node_lifecycle_state::running, 2U,
                      wh::core::errc::timeout)
              .has_value());
  REQUIRE(table
              .update(1U, wh::compose::graph_node_lifecycle_state::completed)
              .has_value());

  auto by_key = table.by_key("a");
  REQUIRE(by_key.has_value());
  REQUIRE(by_key->node_id == 0U);
  REQUIRE(by_key->lifecycle == wh::compose::graph_node_lifecycle_state::running);
  REQUIRE(by_key->attempts == 2U);
  REQUIRE(by_key->last_error == std::optional<wh::core::error_code>{
                                   wh::core::errc::timeout});

  auto by_id = table.by_id(1U);
  REQUIRE(by_id.has_value());
  REQUIRE(by_id->key == "b");
  REQUIRE(by_id->lifecycle ==
          wh::compose::graph_node_lifecycle_state::completed);

  auto states = table.states();
  REQUIRE(states.size() == 2U);
  REQUIRE(states[0].key == "a");
  REQUIRE(states[1].key == "b");
}
