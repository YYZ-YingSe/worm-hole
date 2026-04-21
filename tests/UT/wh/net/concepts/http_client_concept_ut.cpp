#include <catch2/catch_test_macros.hpp>

#include "wh/net/concepts/http_client_concept.hpp"

namespace {

struct concept_http_client {
  auto invoke(const wh::net::http_request_view) const -> wh::net::http_invoke_result;
  auto invoke(const wh::net::http_request &) const -> wh::net::http_invoke_result;
  auto invoke(wh::net::http_request &&) const -> wh::net::http_invoke_result;
  auto invoke_json(const wh::net::http_json_request_view) const -> wh::net::http_invoke_result;
  auto invoke_json(const wh::net::http_json_request &) const -> wh::net::http_invoke_result;
  auto invoke_json(wh::net::http_json_request &&) const -> wh::net::http_invoke_result;
  auto stream(const wh::net::http_stream_request_view) const -> wh::net::http_stream_result;
  auto stream(const wh::net::http_stream_request &) const -> wh::net::http_stream_result;
  auto stream(wh::net::http_stream_request &&) const -> wh::net::http_stream_result;
};

struct incomplete_http_client {
  auto invoke(const wh::net::http_request_view) const -> wh::net::http_invoke_result;
};

static_assert(wh::net::http_client_like<concept_http_client>);
static_assert(!wh::net::http_client_like<incomplete_http_client>);

} // namespace

TEST_CASE("http client concept accepts complete transport adapters",
          "[UT][wh/net/concepts/"
          "http_client_concept.hpp][http_client_like][condition][branch][boundary]") {
  REQUIRE(wh::net::http_client_like<concept_http_client>);
  REQUIRE_FALSE(wh::net::http_client_like<incomplete_http_client>);
}

TEST_CASE("http client concept rejects adapters missing json and stream entrypoints",
          "[UT][wh/net/concepts/"
          "http_client_concept.hpp][http_client_like][condition][branch][boundary][negative]") {
  constexpr bool valid = wh::net::http_client_like<concept_http_client>;
  constexpr bool invalid = wh::net::http_client_like<incomplete_http_client>;
  REQUIRE(valid);
  REQUIRE_FALSE(invalid);
}
