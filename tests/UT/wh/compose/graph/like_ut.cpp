#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/authored/chain.hpp"
#include "wh/compose/graph/like.hpp"

namespace {

struct graph_view_holder {
  wh::compose::graph inner{};

  [[nodiscard]] auto graph_view() const noexcept -> const wh::compose::graph & { return inner; }
};

struct graph_owner_holder {
  wh::compose::graph inner{};

  [[nodiscard]] auto release_graph() && noexcept -> wh::compose::graph { return std::move(inner); }
};

} // namespace

TEST_CASE("graph like concepts detect viewable and owning graph surfaces",
          "[UT][wh/compose/graph/like.hpp][graph_viewable][condition][boundary]") {
  STATIC_REQUIRE(wh::compose::graph_viewable<wh::compose::graph>);
  STATIC_REQUIRE(wh::compose::graph_viewable<wh::compose::chain>);
  STATIC_REQUIRE(wh::compose::graph_viewable<graph_view_holder>);
  STATIC_REQUIRE(!wh::compose::graph_viewable<int>);
  STATIC_REQUIRE(wh::compose::graph_owning<wh::compose::graph &&>);
  STATIC_REQUIRE(wh::compose::graph_owning<wh::compose::chain &&>);
  STATIC_REQUIRE(wh::compose::graph_owning<graph_owner_holder &&>);
  STATIC_REQUIRE(!wh::compose::graph_owning<int>);

  wh::compose::chain chain{};
  const auto &borrowed = wh::compose::detail::borrow_graph(chain);
  REQUIRE_FALSE(borrowed.compiled());
}

TEST_CASE("graph like borrow_graph returns the underlying graph view for graph and wrappers",
          "[UT][wh/compose/graph/like.hpp][borrow_graph][branch]") {
  wh::compose::graph direct{};
  REQUIRE(&wh::compose::detail::borrow_graph(direct) == &direct);

  graph_view_holder holder{};
  REQUIRE(&wh::compose::detail::borrow_graph(holder) == &holder.inner);
}
