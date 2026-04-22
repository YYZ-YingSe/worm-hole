#include <catch2/catch_test_macros.hpp>

#include "wh/adk/detail/shared_state.hpp"

namespace {

struct shared_counter {
  int value{0};
};

} // namespace

TEST_CASE(
    "shared_process_state resolves root state for root and child process states",
    "[UT][wh/adk/detail/shared_state.hpp][shared_process_state][condition][branch][boundary]") {
  wh::compose::graph_process_state root{};
  wh::compose::graph_process_state child{&root};

  REQUIRE(&wh::adk::detail::shared_process_state(root) == &root);
  REQUIRE(&wh::adk::detail::shared_process_state(child) == &root);
}

TEST_CASE("shared_state_ref reports not found before shared state exists",
          "[UT][wh/adk/detail/shared_state.hpp][shared_state_ref][branch][boundary]") {
  wh::compose::graph_process_state root{};

  auto missing = wh::adk::detail::shared_state_ref<shared_counter>(root);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE(
    "emplace_shared_state inserts into shared parent store and replaces prior value",
    "[UT][wh/adk/detail/shared_state.hpp][emplace_shared_state][condition][branch][boundary]") {
  wh::compose::graph_process_state root{};
  wh::compose::graph_process_state child{&root};

  auto inserted = wh::adk::detail::emplace_shared_state<shared_counter>(child, 7);
  REQUIRE(inserted.has_value());
  REQUIRE(inserted.value().get().value == 7);

  auto found = wh::adk::detail::shared_state_ref<shared_counter>(child);
  REQUIRE(found.has_value());
  REQUIRE(found.value().get().value == 7);

  auto overwritten = wh::adk::detail::emplace_shared_state<shared_counter>(root, 11);
  REQUIRE(overwritten.has_value());
  REQUIRE(overwritten.value().get().value == 11);

  auto shared_again = wh::adk::detail::shared_state_ref<shared_counter>(child);
  REQUIRE(shared_again.has_value());
  REQUIRE(shared_again.value().get().value == 11);
}

TEST_CASE("shared_state helpers keep subgraph roots isolated from parent workflow state",
          "[UT][wh/adk/detail/shared_state.hpp][shared_state_ref][workflow_root][boundary]") {
  wh::compose::graph_process_state root{};
  REQUIRE(wh::adk::detail::emplace_shared_state<shared_counter>(root, 5).has_value());

  wh::compose::graph_process_state subgraph{};
  subgraph.set_parent(&root);

  auto inherited = wh::adk::detail::shared_state_ref<shared_counter>(subgraph);
  REQUIRE(inherited.has_error());
  REQUIRE(inherited.error() == wh::core::errc::not_found);

  auto local = wh::adk::detail::emplace_shared_state<shared_counter>(subgraph, 9);
  REQUIRE(local.has_value());
  REQUIRE(local->get().value == 9);

  auto root_again = wh::adk::detail::shared_state_ref<shared_counter>(root);
  REQUIRE(root_again.has_value());
  REQUIRE(root_again->get().value == 5);
}
