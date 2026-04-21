#include <catch2/catch_test_macros.hpp>

#include "wh/schema/document.hpp"

TEST_CASE("document facade exposes document metadata and vector helpers",
          "[UT][wh/schema/document.hpp][document][condition][branch][boundary]") {
  wh::schema::document doc{"hello"};
  doc.set_metadata("flag", true)
      .set_metadata("count", std::int64_t{7})
      .with_score(0.5)
      .with_sub_index("sub-1")
      .with_dsl("dsl")
      .with_extra_info("extra")
      .with_dense_vector({1.0, 2.0})
      .with_sparse_vector({{3U, 4.0}});

  REQUIRE(doc.content() == "hello");
  REQUIRE(doc.has_metadata("flag"));
  REQUIRE(doc.metadata_or<bool>("flag") == true);
  REQUIRE(doc.metadata_ptr<std::int64_t>("count") != nullptr);
  REQUIRE(doc.score() == 0.5);
  REQUIRE(doc.sub_index() == "sub-1");
  REQUIRE(doc.dsl() == "dsl");
  REQUIRE(doc.extra_info() == "extra");
  REQUIRE(doc.get_dense_vector() == wh::schema::dense_vector{1.0, 2.0});
  REQUIRE(doc.get_sparse_vector() == wh::schema::sparse_vector{{3U, 4.0}});

  auto missing = doc.metadata_cref<std::string>("missing");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("document facade keeps default document state empty and typed fallbacks stable",
          "[UT][wh/schema/document.hpp][document][condition][branch][boundary][default]") {
  wh::schema::document doc{};
  REQUIRE(doc.content().empty());
  REQUIRE_FALSE(doc.has_metadata("missing"));
  REQUIRE(doc.score() == 0.0);
  REQUIRE(doc.sub_index().empty());
  REQUIRE(doc.dsl().empty());
  REQUIRE(doc.extra_info().empty());
  REQUIRE(doc.get_dense_vector().empty());
  REQUIRE(doc.get_sparse_vector().empty());
}
