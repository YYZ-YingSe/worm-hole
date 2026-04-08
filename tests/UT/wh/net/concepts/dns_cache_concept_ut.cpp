#include <catch2/catch_test_macros.hpp>

#include "wh/net/concepts/dns_cache_concept.hpp"

namespace {

struct concept_dns_cache {
  auto lookup(const wh::net::dns_lookup_request &) const -> wh::net::dns_lookup_result;
  auto lookup(wh::net::dns_lookup_request &&) const -> wh::net::dns_lookup_result;
  auto lookup(const wh::net::dns_lookup_request_view) const
      -> wh::net::dns_lookup_result;
  auto invalidate(const std::string_view) const -> wh::net::dns_invalidate_result;
};

struct incomplete_dns_cache {
  auto lookup(const wh::net::dns_lookup_request &) const -> wh::net::dns_lookup_result;
};

static_assert(wh::net::dns_cache_like<concept_dns_cache>);
static_assert(!wh::net::dns_cache_like<incomplete_dns_cache>);

} // namespace

TEST_CASE("dns cache concept accepts complete cache adapters",
          "[UT][wh/net/concepts/dns_cache_concept.hpp][dns_cache_like][condition][branch][boundary]") {
  REQUIRE(wh::net::dns_cache_like<concept_dns_cache>);
  REQUIRE_FALSE(wh::net::dns_cache_like<incomplete_dns_cache>);
}

TEST_CASE("dns cache concept rejects adapters without invalidation entrypoint",
          "[UT][wh/net/concepts/dns_cache_concept.hpp][dns_cache_like][condition][branch][boundary][negative]") {
  constexpr bool valid = wh::net::dns_cache_like<concept_dns_cache>;
  constexpr bool invalid = wh::net::dns_cache_like<incomplete_dns_cache>;
  REQUIRE(valid);
  REQUIRE_FALSE(invalid);
}
