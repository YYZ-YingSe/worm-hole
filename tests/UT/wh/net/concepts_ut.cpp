#include <catch2/catch_test_macros.hpp>

#include "wh/net/concepts.hpp"

namespace {

struct facade_http_client {
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

struct facade_sse_parser {
  auto parse(const wh::net::sse_parse_request &) const -> wh::net::sse_parse_result;
  auto parse(wh::net::sse_parse_request &&) const -> wh::net::sse_parse_result;
  auto parse(const wh::net::sse_parse_request_view) const -> wh::net::sse_parse_result;
};

struct incomplete_facade_http_client {
  auto invoke(const wh::net::http_request_view) const -> wh::net::http_invoke_result;
};

} // namespace

TEST_CASE("net concepts facade includes all adapter contracts",
          "[UT][wh/net/concepts.hpp][http_client_like][condition][branch][boundary]") {
  REQUIRE(wh::net::http_client_like<facade_http_client>);
  REQUIRE_FALSE(wh::net::http_client_like<incomplete_facade_http_client>);
}

TEST_CASE("net concepts facade exports parser concept names from the umbrella header",
          "[UT][wh/net/concepts.hpp][sse_parser_like][condition][branch][boundary]") {
  REQUIRE(wh::net::sse_parser_like<facade_sse_parser>);
  REQUIRE_FALSE(wh::net::sse_parser_like<incomplete_facade_http_client>);
}
