#include <catch2/catch_test_macros.hpp>

#include "wh/tool/catalog.hpp"

TEST_CASE("tool catalog cache handshakes caches refreshes and invalidates",
          "[UT][wh/tool/catalog.hpp][tool_catalog_cache::load][branch][boundary]") {
  int handshake_calls = 0;
  int fetch_calls = 0;

  wh::tool::tool_catalog_source source{};
  source.handshake = [&]() -> wh::core::result<void> {
    ++handshake_calls;
    return {};
  };
  source.fetch_catalog =
      [&]() -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> {
    ++fetch_calls;
    return std::vector<wh::schema::tool_schema_definition>{
        {.name = "search", .description = "lookup"},
    };
  };

  wh::tool::tool_catalog_cache cache{source};
  auto first = cache.load();
  REQUIRE(first.has_value());
  REQUIRE(first.value().size() == 1U);
  REQUIRE(handshake_calls == 1);
  REQUIRE(fetch_calls == 1);

  auto cached = cache.load();
  REQUIRE(cached.has_value());
  REQUIRE(cached.value().front().name == "search");
  REQUIRE(handshake_calls == 1);
  REQUIRE(fetch_calls == 1);

  auto refreshed = cache.load({.refresh = true});
  REQUIRE(refreshed.has_value());
  REQUIRE(handshake_calls == 2);
  REQUIRE(fetch_calls == 2);

  cache.invalidate();
  auto invalidated = cache.load();
  REQUIRE(invalidated.has_value());
  REQUIRE(handshake_calls == 3);
  REQUIRE(fetch_calls == 3);
}

TEST_CASE("tool catalog cache surfaces missing fetcher and handshake errors",
          "[UT][wh/tool/catalog.hpp][tool_catalog_cache::set_source][branch]") {
  wh::tool::tool_catalog_cache cache{};
  auto missing = cache.load();
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  wh::tool::tool_catalog_source failing{};
  failing.handshake = []() -> wh::core::result<void> {
    return wh::core::result<void>::failure(wh::core::errc::canceled);
  };
  failing.fetch_catalog =
      []() -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> {
    return std::vector<wh::schema::tool_schema_definition>{};
  };
  cache.set_source(std::move(failing));

  auto failed = cache.load();
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::canceled);
}

TEST_CASE("tool catalog cache skips absent handshake and invalidates replaced sources",
          "[UT][wh/tool/catalog.hpp][tool_catalog_cache::load][condition][boundary]") {
  int first_fetches = 0;
  int second_fetches = 0;

  wh::tool::tool_catalog_cache cache{wh::tool::tool_catalog_source{
      .fetch_catalog = [&]() -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> {
        ++first_fetches;
        return std::vector<wh::schema::tool_schema_definition>{
            {.name = "search", .description = "lookup"},
        };
      }}};

  auto first = cache.load();
  REQUIRE(first.has_value());
  REQUIRE(first.value().front().name == "search");
  REQUIRE(first_fetches == 1);

  cache.set_source(wh::tool::tool_catalog_source{
      .fetch_catalog = [&]() -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> {
        ++second_fetches;
        return std::vector<wh::schema::tool_schema_definition>{
            {.name = "rewrite", .description = "transform"},
        };
      }});

  auto replaced = cache.load();
  REQUIRE(replaced.has_value());
  REQUIRE(replaced.value().front().name == "rewrite");
  REQUIRE(first_fetches == 1);
  REQUIRE(second_fetches == 1);
}
