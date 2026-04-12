#include <catch2/catch_test_macros.hpp>

#include "wh/net/types/http_client_types.hpp"

TEST_CASE("http client types project owned requests into borrowed views",
          "[UT][wh/net/types/http_client_types.hpp][make_http_request_view][condition][branch][boundary]") {
  wh::net::http_request request{};
  request.method = wh::net::http_method::post;
  request.url = "https://example.com";
  request.headers.push_back({"x-id", "1"});
  request.body = "body";

  const auto view = wh::net::make_http_request_view(request);
  REQUIRE(view.method == wh::net::http_method::post);
  REQUIRE(view.url == "https://example.com");
  REQUIRE(view.headers.size() == 1U);
  REQUIRE(view.body == "body");

  wh::net::http_json_request json{};
  json.url = "https://example.com/json";
  json.json_body = R"({"a":1})";
  const auto json_view = wh::net::make_http_json_request_view(json);
  REQUIRE(json_view.url == "https://example.com/json");
  REQUIRE(json_view.json_body == R"({"a":1})");

  wh::net::http_stream_request stream{};
  stream.request = request;
  stream.protocol = wh::net::http_stream_protocol::sse;
  const auto stream_view = wh::net::make_http_stream_request_view(stream);
  REQUIRE(stream_view.request.url == "https://example.com");
  REQUIRE(stream_view.protocol == wh::net::http_stream_protocol::sse);
}

TEST_CASE("http client transport types default errors methods and responses stay stable",
          "[UT][wh/net/types/http_client_types.hpp][transport_error][condition][branch][boundary]") {
  wh::net::transport_error error{};
  REQUIRE(error.kind == wh::net::transport_error_kind::unknown);
  REQUIRE(error.code == wh::core::error_code{});
  REQUIRE_FALSE(error.retryable);
  REQUIRE_FALSE(error.status_code.has_value());
  REQUIRE(error.provider_code.empty());
  REQUIRE(error.message.empty());
  REQUIRE(error.diagnostic_context.empty());

  wh::net::http_response response{};
  REQUIRE(response.status_code == 0);
  REQUIRE(response.headers.empty());
  REQUIRE(response.body.empty());

  REQUIRE(wh::net::http_method::get != wh::net::http_method::post);
  REQUIRE(wh::net::http_stream_protocol::sse !=
          wh::net::http_stream_protocol::raw);
}
