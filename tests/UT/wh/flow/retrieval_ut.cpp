#include <catch2/catch_test_macros.hpp>

#include "wh/flow/retrieval.hpp"

TEST_CASE("flow retrieval facade exports stable helper keys and defaults",
          "[UT][wh/flow/retrieval.hpp][request_node_key][condition][branch][boundary]") {
  REQUIRE(wh::flow::retrieval::parent_id_metadata_key == wh::document::parent_id_metadata_key);
  REQUIRE(wh::flow::retrieval::detail::multi_query::request_node_key() == "multi_query_request");
  REQUIRE(wh::flow::retrieval::detail::router::router_stage_name == "Router");
}

TEST_CASE("flow retrieval facade exports multi-query fusion helpers through the public header",
          "[UT][wh/flow/retrieval.hpp][first_hit_fusion][condition][branch][boundary]") {
  wh::flow::retrieval::detail::multi_query::first_hit_fusion fusion{};
  auto fused = fusion({});

  REQUIRE(fused.has_value());
  REQUIRE(fused.value().empty());
}
