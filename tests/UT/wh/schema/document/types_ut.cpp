#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/document/types.hpp"

TEST_CASE(
    "document types expose reserved keys metadata accessors and typed result branches",
    "[UT][wh/schema/document/types.hpp][document::set_metadata][condition][branch][boundary]") {
  REQUIRE(wh::schema::document_metadata_keys::score == "_score");
  REQUIRE(wh::schema::document_metadata_keys::sub_index == "_sub_index");
  REQUIRE(wh::schema::document_metadata_keys::dsl == "_dsl");
  REQUIRE(wh::schema::document_metadata_keys::extra_info == "_extra_info");
  REQUIRE(wh::schema::document_metadata_keys::dense_vector == "_dense_vector");
  REQUIRE(wh::schema::document_metadata_keys::sparse_vector == "_sparse_vector");

  wh::schema::document doc{"hello"};
  REQUIRE(doc.content() == "hello");
  REQUIRE_FALSE(doc.has_metadata("missing"));
  REQUIRE(doc.metadata() == nullptr);

  doc.with_score(0.95)
      .with_sub_index("segment-1")
      .with_dsl("lang:cpp")
      .with_extra_info("unit-test")
      .with_dense_vector({1.0, 2.0, 3.0})
      .with_sparse_vector({{0U, 0.4}, {3U, 0.8}})
      .set_metadata("flag", true)
      .set_metadata("count", std::int64_t{7});

  REQUIRE(doc.metadata() != nullptr);
  REQUIRE(doc.has_metadata("flag"));
  REQUIRE(std::abs(doc.score() - 0.95) < 1e-12);
  REQUIRE(doc.sub_index() == "segment-1");
  REQUIRE(doc.dsl() == "lang:cpp");
  REQUIRE(doc.extra_info() == "unit-test");
  REQUIRE(doc.get_dense_vector().size() == 3U);
  REQUIRE(doc.get_sparse_vector().size() == 2U);

  REQUIRE(doc.metadata_or<bool>("flag", false));
  REQUIRE(doc.metadata_or<std::string>("missing", "fallback") == "fallback");
  REQUIRE(doc.metadata_or<std::int64_t>(wh::schema::document_metadata_keys::sub_index, 9) == 9);
  REQUIRE(doc.metadata_ptr<bool>("flag") != nullptr);
  REQUIRE(*doc.metadata_ptr<bool>("flag"));
  REQUIRE(doc.metadata_ptr<double>("flag") == nullptr);

  auto ok_ref = doc.metadata_cref<std::string>(wh::schema::document_metadata_keys::sub_index);
  REQUIRE(ok_ref.has_value());
  REQUIRE(ok_ref.value().get() == "segment-1");

  auto missing_ref = doc.metadata_cref<std::string>("missing");
  REQUIRE(missing_ref.has_error());
  REQUIRE(missing_ref.error() == wh::core::errc::not_found);

  auto mismatch_ref =
      doc.metadata_cref<std::int64_t>(wh::schema::document_metadata_keys::sub_index);
  REQUIRE(mismatch_ref.has_error());
  REQUIRE(mismatch_ref.error() == wh::core::errc::type_mismatch);

  doc.set_content("updated");
  REQUIRE(doc.content() == "updated");
}

TEST_CASE(
    "document type aliases expose expected metadata container surface",
    "[UT][wh/schema/document/types.hpp][document_metadata_map][condition][branch][boundary]") {
  STATIC_REQUIRE(std::same_as<wh::schema::sparse_vector_item, std::pair<std::uint32_t, double>>);

  wh::schema::document_metadata_map metadata{};
  metadata.insert_or_assign("names", std::vector<std::string>{"a", "b"});
  metadata.insert_or_assign("none", nullptr);
  REQUIRE(metadata.size() == 2U);
  REQUIRE(std::holds_alternative<std::vector<std::string>>(metadata.at("names")));
  REQUIRE(std::holds_alternative<std::nullptr_t>(metadata.at("none")));
}
