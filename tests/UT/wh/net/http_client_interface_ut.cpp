#include <catch2/catch_test_macros.hpp>

#include "wh/net/http_client_interface.hpp"

namespace {

struct forwarding_http_client {
  auto invoke(const wh::net::http_request_view) const -> wh::net::http_invoke_result {
    return wh::net::http_response{.status_code = 200, .body = "view"};
  }

  auto invoke(const wh::net::http_request &) const -> wh::net::http_invoke_result {
    return wh::net::http_response{.status_code = 201, .body = "owned"};
  }

  auto invoke(wh::net::http_request &&) const -> wh::net::http_invoke_result {
    return wh::net::http_response{.status_code = 202, .body = "moved"};
  }

  auto invoke_json(const wh::net::http_json_request_view) const -> wh::net::http_invoke_result {
    return wh::net::http_response{.status_code = 210, .body = "json-view"};
  }

  auto invoke_json(const wh::net::http_json_request &) const -> wh::net::http_invoke_result {
    return wh::net::http_response{.status_code = 211, .body = "json-owned"};
  }

  auto invoke_json(wh::net::http_json_request &&) const -> wh::net::http_invoke_result {
    return wh::net::http_response{.status_code = 212, .body = "json-moved"};
  }

  auto stream(const wh::net::http_stream_request_view) const -> wh::net::http_stream_result {
    return wh::net::http_stream_result::failure(wh::net::transport_error{.message = "stream-view"});
  }

  auto stream(const wh::net::http_stream_request &) const -> wh::net::http_stream_result {
    return wh::net::http_stream_result::failure(
        wh::net::transport_error{.message = "stream-owned"});
  }

  auto stream(wh::net::http_stream_request &&) const -> wh::net::http_stream_result {
    return wh::net::http_stream_result::failure(
        wh::net::transport_error{.message = "stream-moved"});
  }
};

static_assert(wh::net::http_client_like<forwarding_http_client>);

} // namespace

TEST_CASE("http client interface forwards to matching overload families",
          "[UT][wh/net/http_client_interface.hpp][invoke_http][branch][boundary]") {
  const forwarding_http_client client{};

  wh::net::http_request request{};
  request.url = "https://example.com";
  const auto view = wh::net::make_http_request_view(request);

  auto invoke_view = wh::net::invoke_http(client, view);
  REQUIRE(invoke_view.has_value());
  REQUIRE(invoke_view.value().body == "view");

  auto invoke_owned = wh::net::invoke_http(client, request);
  REQUIRE(invoke_owned.has_value());
  REQUIRE(invoke_owned.value().body == "owned");

  auto invoke_moved = wh::net::invoke_http(client, wh::net::http_request{});
  REQUIRE(invoke_moved.has_value());
  REQUIRE(invoke_moved.value().body == "moved");

  wh::net::http_json_request json{};
  auto json_view = wh::net::invoke_http_json(client, wh::net::make_http_json_request_view(json));
  REQUIRE(json_view.has_value());
  REQUIRE(json_view.value().body == "json-view");

  auto json_owned = wh::net::invoke_http_json(client, json);
  REQUIRE(json_owned.has_value());
  REQUIRE(json_owned.value().body == "json-owned");

  auto json_moved = wh::net::invoke_http_json(client, wh::net::http_json_request{});
  REQUIRE(json_moved.has_value());
  REQUIRE(json_moved.value().body == "json-moved");
}

TEST_CASE("http client interface forwards stream calls to matching overloads",
          "[UT][wh/net/http_client_interface.hpp][stream_http][branch]") {
  const forwarding_http_client client{};

  wh::net::http_stream_request request{};
  request.protocol = wh::net::http_stream_protocol::raw;
  auto request_view = wh::net::make_http_stream_request_view(request);

  auto view = wh::net::stream_http(client, request_view);
  REQUIRE(view.has_error());
  REQUIRE(view.error().message == "stream-view");

  auto owned = wh::net::stream_http(client, request);
  REQUIRE(owned.has_error());
  REQUIRE(owned.error().message == "stream-owned");

  auto moved = wh::net::stream_http(client, wh::net::http_stream_request{});
  REQUIRE(moved.has_error());
  REQUIRE(moved.error().message == "stream-moved");
}

TEST_CASE("http client interface keeps overload families distinct for const inputs",
          "[UT][wh/net/http_client_interface.hpp][invoke_http_json][condition][branch][boundary]") {
  const forwarding_http_client client{};

  const wh::net::http_request request{};
  const auto owned = wh::net::invoke_http(client, request);
  REQUIRE(owned.has_value());
  REQUIRE(owned.value().status_code == 201);

  const wh::net::http_json_request json_request{};
  const auto json_owned = wh::net::invoke_http_json(client, json_request);
  REQUIRE(json_owned.has_value());
  REQUIRE(json_owned.value().status_code == 211);
}
