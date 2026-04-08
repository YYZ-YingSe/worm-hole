#include <catch2/catch_test_macros.hpp>

#include "wh/document/keys.hpp"

TEST_CASE("document parent id metadata key exposes the stable reserved name",
          "[UT][wh/document/keys.hpp][parent_id_metadata_key][boundary]") {
  REQUIRE(wh::document::parent_id_metadata_key == "__parent_id__");
  REQUIRE_FALSE(wh::document::parent_id_metadata_key.empty());
}

TEST_CASE("document sub id metadata key stays distinct from parent id key",
          "[UT][wh/document/keys.hpp][sub_id_metadata_key][condition][branch][boundary]") {
  REQUIRE(wh::document::sub_id_metadata_key == "__sub_id__");
  REQUIRE_FALSE(wh::document::sub_id_metadata_key.empty());
  REQUIRE(wh::document::parent_id_metadata_key !=
          wh::document::sub_id_metadata_key);
}
