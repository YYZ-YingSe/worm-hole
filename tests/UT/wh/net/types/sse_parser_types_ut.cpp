#include <catch2/catch_test_macros.hpp>

#include "wh/net/types/sse_parser_types.hpp"

TEST_CASE("sse parser types project owned requests into borrowed views",
          "[UT][wh/net/types/"
          "sse_parser_types.hpp][make_sse_parse_request_view][condition][branch][boundary]") {
  wh::net::sse_parse_request request{};
  request.bytes = {std::byte{0x41}, std::byte{0x42}};
  request.end_of_stream = true;

  const auto view = wh::net::make_sse_parse_request_view(request);
  REQUIRE(view.bytes.size() == 2U);
  REQUIRE(view.end_of_stream);

  wh::net::http_stream_event event{};
  event.protocol = wh::net::http_stream_protocol::sse;
  event.payload = wh::net::sse_event{.event = "message", .data = "payload"};
  REQUIRE(std::holds_alternative<wh::net::sse_event>(event.payload));
}

TEST_CASE(
    "sse parser types default output and raw payload variants stay distinct",
    "[UT][wh/net/types/sse_parser_types.hpp][sse_parse_output][condition][branch][boundary]") {
  wh::net::sse_parse_output output{};
  REQUIRE(output.events.empty());
  REQUIRE_FALSE(output.end_of_stream);

  wh::net::http_stream_event raw{};
  raw.protocol = wh::net::http_stream_protocol::raw;
  raw.payload = wh::net::raw_stream_chunk{.bytes = {std::byte{0x01}, std::byte{0x02}}};
  REQUIRE(std::holds_alternative<wh::net::raw_stream_chunk>(raw.payload));
  REQUIRE(std::get<wh::net::raw_stream_chunk>(raw.payload).bytes.size() == 2U);
}
