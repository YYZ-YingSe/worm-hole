#include <catch2/catch_test_macros.hpp>

#include "wh/flow/indexing.hpp"

TEST_CASE("flow indexing facade exports parent and sub-id metadata keys",
          "[UT][wh/flow/indexing.hpp][parent_id_metadata_key][condition][branch][boundary]") {
  REQUIRE(wh::flow::indexing::parent_id_metadata_key == wh::document::parent_id_metadata_key);
  REQUIRE(wh::flow::indexing::sub_id_metadata_key == wh::document::sub_id_metadata_key);
}

TEST_CASE("flow indexing facade keeps indexing metadata keys distinct",
          "[UT][wh/flow/indexing.hpp][sub_id_metadata_key][condition][branch][boundary]") {
  REQUIRE(wh::flow::indexing::parent_id_metadata_key != wh::flow::indexing::sub_id_metadata_key);
}
