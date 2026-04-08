#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/detail/contract.hpp"
#include "wh/schema/stream/pipe.hpp"

namespace {

using graph_reader_t = wh::compose::graph_stream_reader;
using graph_reader_result_t = wh::core::result<graph_reader_t>;
using pipe_reader_t = wh::schema::stream::pipe_stream_reader<wh::compose::graph_value>;
using canonical_sender_t = decltype(stdexec::just(graph_reader_result_t{}));
using non_canonical_sender_t = decltype(stdexec::just(wh::core::result<int>{1}));

static_assert(wh::compose::detail::canonical_reader<graph_reader_t>);
static_assert(wh::compose::detail::canonical_reader<pipe_reader_t>);
static_assert(!wh::compose::detail::canonical_reader<int>);

static_assert(
    wh::compose::detail::canonical_stream_status<graph_reader_result_t>);
static_assert(
    !wh::compose::detail::canonical_stream_status<wh::core::result<int>>);

static_assert(
    wh::compose::detail::set_value_signature<stdexec::set_value_t(int)>::value);
static_assert(std::same_as<
              wh::compose::detail::set_value_signature<stdexec::set_value_t(
                  int)>::result_type,
              int>);
static_assert(!wh::compose::detail::set_value_signature<
              stdexec::set_error_t(std::exception_ptr)>::value);

static_assert(
    wh::compose::detail::canonical_stream_sender_signature_ok<
        stdexec::set_value_t(graph_reader_result_t),
        stdexec::set_stopped_t()>());
static_assert(
    !wh::compose::detail::canonical_stream_sender_signature_ok<
        stdexec::set_value_t(wh::core::result<int>),
        stdexec::set_stopped_t()>());
static_assert(
    !wh::compose::detail::canonical_stream_sender_signature_ok<
        stdexec::set_value_t(graph_reader_result_t),
        stdexec::set_value_t(graph_reader_result_t)>());

static_assert(wh::compose::detail::canonical_stream_sender<
              canonical_sender_t>);
static_assert(!wh::compose::detail::canonical_stream_sender<
              non_canonical_sender_t>);

static_assert(
    wh::compose::detail::typed_request<wh::compose::node_contract::value, int>);
static_assert(!wh::compose::detail::typed_request<wh::compose::node_contract::value,
                                                  graph_reader_t>);
static_assert(wh::compose::detail::typed_request<wh::compose::node_contract::stream,
                                                 graph_reader_t>);

static_assert(wh::compose::detail::exact_value_payload<int>);
static_assert(!wh::compose::detail::exact_value_payload<graph_reader_t>);

static_assert(wh::compose::detail::typed_response<
              wh::compose::node_contract::value, int>);
static_assert(!wh::compose::detail::typed_response<
              wh::compose::node_contract::value, graph_reader_t>);
static_assert(wh::compose::detail::typed_response<
              wh::compose::node_contract::stream, graph_reader_t>);

} // namespace

TEST_CASE("contract concepts classify canonical readers stream statuses and senders",
          "[UT][wh/compose/node/detail/contract.hpp][canonical_reader][condition][branch][boundary]") {
  STATIC_REQUIRE(wh::compose::detail::canonical_reader<graph_reader_t>);
  STATIC_REQUIRE(wh::compose::detail::canonical_reader<pipe_reader_t>);
  STATIC_REQUIRE_FALSE(wh::compose::detail::canonical_reader<int>);

  STATIC_REQUIRE(
      wh::compose::detail::canonical_stream_status<graph_reader_result_t>);
  STATIC_REQUIRE_FALSE(
      wh::compose::detail::canonical_stream_status<wh::core::result<int>>);

  STATIC_REQUIRE(
      wh::compose::detail::canonical_stream_sender<canonical_sender_t>);
  STATIC_REQUIRE_FALSE(
      wh::compose::detail::canonical_stream_sender<non_canonical_sender_t>);
}

TEST_CASE("set_value_signature helpers detect set_value completion signatures only",
          "[UT][wh/compose/node/detail/contract.hpp][set_value_signature][condition][branch]") {
  REQUIRE(
      wh::compose::detail::set_value_signature<stdexec::set_value_t(int)>::value);
  REQUIRE_FALSE(wh::compose::detail::set_value_signature<
                stdexec::set_error_t(std::exception_ptr)>::value);
  STATIC_REQUIRE(std::same_as<
                 wh::compose::detail::set_value_signature<stdexec::set_value_t(
                     int)>::result_type,
                 int>);
}

TEST_CASE("canonical_stream_sender_signature_ok accepts exactly one canonical set_value result",
          "[UT][wh/compose/node/detail/contract.hpp][canonical_stream_sender_signature_ok][condition][branch]") {
  REQUIRE(wh::compose::detail::canonical_stream_sender_signature_ok<
          stdexec::set_value_t(graph_reader_result_t),
          stdexec::set_stopped_t()>());
  REQUIRE_FALSE(wh::compose::detail::canonical_stream_sender_signature_ok<
                stdexec::set_value_t(wh::core::result<int>),
                stdexec::set_stopped_t()>());
  REQUIRE_FALSE(wh::compose::detail::canonical_stream_sender_signature_ok<
                stdexec::set_value_t(graph_reader_result_t),
                stdexec::set_value_t(graph_reader_result_t)>());
}

TEST_CASE("typed request and response concepts distinguish value and stream contracts",
          "[UT][wh/compose/node/detail/contract.hpp][typed_response][condition][branch][boundary]") {
  STATIC_REQUIRE(
      wh::compose::detail::typed_request<wh::compose::node_contract::value, int>);
  STATIC_REQUIRE_FALSE(wh::compose::detail::typed_request<
                       wh::compose::node_contract::value, graph_reader_t>);
  STATIC_REQUIRE(wh::compose::detail::typed_request<
                 wh::compose::node_contract::stream, graph_reader_t>);

  STATIC_REQUIRE(wh::compose::detail::exact_value_payload<int>);
  STATIC_REQUIRE_FALSE(
      wh::compose::detail::exact_value_payload<graph_reader_t>);

  STATIC_REQUIRE(
      wh::compose::detail::typed_response<wh::compose::node_contract::value, int>);
  STATIC_REQUIRE_FALSE(wh::compose::detail::typed_response<
                       wh::compose::node_contract::value, graph_reader_t>);
  STATIC_REQUIRE(wh::compose::detail::typed_response<
                 wh::compose::node_contract::stream, graph_reader_t>);
}

TEST_CASE("typed gate factories map declared contracts into exact and reader gates",
          "[UT][wh/compose/node/detail/contract.hpp][typed_output_gate][condition][branch][boundary]") {
  const auto value_input =
      wh::compose::detail::typed_input_gate<wh::compose::node_contract::value,
                                            int>();
  REQUIRE(value_input.kind == wh::compose::input_gate_kind::value_exact);
  REQUIRE(value_input.value == wh::compose::value_gate::exact<int>());

  const auto stream_input =
      wh::compose::detail::typed_input_gate<wh::compose::node_contract::stream,
                                            graph_reader_t>();
  REQUIRE(stream_input.kind == wh::compose::input_gate_kind::reader);
  REQUIRE(stream_input.value.empty());

  const auto value_output =
      wh::compose::detail::typed_output_gate<wh::compose::node_contract::value,
                                             int>();
  REQUIRE(value_output.kind == wh::compose::output_gate_kind::value_exact);
  REQUIRE(value_output.value == wh::compose::value_gate::exact<int>());

  const auto stream_output =
      wh::compose::detail::typed_output_gate<wh::compose::node_contract::stream,
                                             graph_reader_t>();
  REQUIRE(stream_output.kind == wh::compose::output_gate_kind::reader);
  REQUIRE(stream_output.value.empty());
}
