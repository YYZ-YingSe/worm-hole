#include <catch2/catch_test_macros.hpp>

#include "wh/flow.hpp"

TEST_CASE("flow facade exports indexing metadata entrypoints",
          "[UT][wh/flow.hpp][indexing][condition][branch][boundary]") {
  REQUIRE(wh::flow::indexing::parent_id_metadata_key == wh::document::parent_id_metadata_key);
  REQUIRE(wh::flow::indexing::sub_id_metadata_key == wh::document::sub_id_metadata_key);
}

TEST_CASE("flow facade exports retrieval helpers through the public header",
          "[UT][wh/flow.hpp][retrieval][condition][branch][boundary]") {
  REQUIRE(wh::flow::retrieval::parent_id_metadata_key == wh::document::parent_id_metadata_key);
  REQUIRE(wh::flow::retrieval::detail::router::router_stage_name == "Router");
}
