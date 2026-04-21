#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/node/detail/lambda_concepts.hpp"

namespace {

struct sync_value_lambda {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_value> {
    return wh::compose::graph_value{1};
  }
};

struct async_value_sender {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{2}});
  }
};

struct sync_map_lambda {
  auto operator()(wh::compose::graph_value_map &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_value_map> {
    return wh::compose::graph_value_map{};
  }
};

struct async_map_sender {
  auto operator()(wh::compose::graph_value_map &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(
        wh::core::result<wh::compose::graph_value_map>{wh::compose::graph_value_map{}});
  }
};

struct sync_value_stream_lambda {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_stream_reader> {
    return wh::compose::make_single_value_stream_reader(3);
  }
};

struct async_value_stream_sender {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(wh::compose::make_single_value_stream_reader(4));
  }
};

struct sync_stream_value_lambda {
  auto operator()(wh::compose::graph_stream_reader, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_value> {
    return wh::compose::graph_value{5};
  }
};

struct async_stream_value_sender {
  auto operator()(wh::compose::graph_stream_reader, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{6}});
  }
};

struct sync_stream_stream_lambda {
  auto operator()(wh::compose::graph_stream_reader, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_stream_reader> {
    return wh::compose::make_single_value_stream_reader(7);
  }
};

struct async_stream_stream_sender {
  auto operator()(wh::compose::graph_stream_reader, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(wh::compose::make_single_value_stream_reader(8));
  }
};

struct wrong_lambda {
  auto operator()(int) const -> int { return 0; }
};

static_assert(wh::compose::value_lambda<sync_value_lambda>);
static_assert(wh::compose::value_sender<async_value_sender>);
static_assert(wh::compose::map_lambda<sync_map_lambda>);
static_assert(wh::compose::map_sender<async_map_sender>);
static_assert(wh::compose::value_stream_lambda<sync_value_stream_lambda>);
static_assert(wh::compose::value_stream_sender<async_value_stream_sender>);
static_assert(wh::compose::stream_value_lambda<sync_stream_value_lambda>);
static_assert(wh::compose::stream_value_sender<async_stream_value_sender>);
static_assert(wh::compose::stream_stream_lambda<sync_stream_stream_lambda>);
static_assert(wh::compose::stream_stream_sender<async_stream_stream_sender>);
static_assert(!wh::compose::value_lambda<wrong_lambda>);
static_assert(!wh::compose::stream_stream_sender<wrong_lambda>);

static_assert(wh::compose::lambda_contract<wh::compose::node_exec_mode::sync,
                                           wh::compose::node_contract::value,
                                           wh::compose::node_contract::value, sync_value_lambda>);
static_assert(wh::compose::lambda_contract<wh::compose::node_exec_mode::async,
                                           wh::compose::node_contract::value,
                                           wh::compose::node_contract::value, async_map_sender>);
static_assert(wh::compose::lambda_contract<
              wh::compose::node_exec_mode::sync, wh::compose::node_contract::value,
              wh::compose::node_contract::stream, sync_value_stream_lambda>);
static_assert(wh::compose::lambda_contract<
              wh::compose::node_exec_mode::async, wh::compose::node_contract::value,
              wh::compose::node_contract::stream, async_value_stream_sender>);
static_assert(wh::compose::lambda_contract<
              wh::compose::node_exec_mode::sync, wh::compose::node_contract::stream,
              wh::compose::node_contract::value, sync_stream_value_lambda>);
static_assert(wh::compose::lambda_contract<
              wh::compose::node_exec_mode::async, wh::compose::node_contract::stream,
              wh::compose::node_contract::value, async_stream_value_sender>);
static_assert(wh::compose::lambda_contract<
              wh::compose::node_exec_mode::sync, wh::compose::node_contract::stream,
              wh::compose::node_contract::stream, sync_stream_stream_lambda>);
static_assert(wh::compose::lambda_contract<
              wh::compose::node_exec_mode::async, wh::compose::node_contract::stream,
              wh::compose::node_contract::stream, async_stream_stream_sender>);
static_assert(!wh::compose::lambda_contract<wh::compose::node_exec_mode::sync,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_contract::stream, wrong_lambda>);

} // namespace

TEST_CASE(
    "lambda concepts accept only matching value map and stream signatures",
    "[UT][wh/compose/node/detail/lambda_concepts.hpp][value_lambda][condition][branch][boundary]") {
  STATIC_REQUIRE(wh::compose::value_lambda<sync_value_lambda>);
  STATIC_REQUIRE(wh::compose::value_sender<async_value_sender>);
  STATIC_REQUIRE(wh::compose::map_lambda<sync_map_lambda>);
  STATIC_REQUIRE(wh::compose::map_sender<async_map_sender>);

  REQUIRE(wh::compose::lambda_contract_supported_v<
          wh::compose::node_exec_mode::sync, wh::compose::node_contract::value,
          wh::compose::node_contract::value, sync_value_lambda>);
  REQUIRE(wh::compose::lambda_contract_supported_v<
          wh::compose::node_exec_mode::async, wh::compose::node_contract::value,
          wh::compose::node_contract::value, async_map_sender>);
}

TEST_CASE(
    "lambda concepts accept canonical value-stream and stream-value signatures",
    "[UT][wh/compose/node/detail/lambda_concepts.hpp][value_stream_lambda][condition][branch]") {
  STATIC_REQUIRE(wh::compose::value_stream_lambda<sync_value_stream_lambda>);
  STATIC_REQUIRE(wh::compose::value_stream_sender<async_value_stream_sender>);
  STATIC_REQUIRE(wh::compose::stream_value_lambda<sync_stream_value_lambda>);
  STATIC_REQUIRE(wh::compose::stream_value_sender<async_stream_value_sender>);

  REQUIRE(wh::compose::lambda_contract_supported_v<
          wh::compose::node_exec_mode::sync, wh::compose::node_contract::value,
          wh::compose::node_contract::stream, sync_value_stream_lambda>);
  REQUIRE(wh::compose::lambda_contract_supported_v<
          wh::compose::node_exec_mode::async, wh::compose::node_contract::stream,
          wh::compose::node_contract::value, async_stream_value_sender>);
}

TEST_CASE("lambda concepts accept canonical stream-stream signatures and reject mismatches",
          "[UT][wh/compose/node/detail/lambda_concepts.hpp][lambda_contract][condition][branch]") {
  STATIC_REQUIRE(wh::compose::stream_stream_lambda<sync_stream_stream_lambda>);
  STATIC_REQUIRE(wh::compose::stream_stream_sender<async_stream_stream_sender>);
  STATIC_REQUIRE_FALSE(wh::compose::value_lambda<wrong_lambda>);
  STATIC_REQUIRE_FALSE(wh::compose::stream_stream_sender<wrong_lambda>);

  REQUIRE(wh::compose::lambda_contract_supported_v<
          wh::compose::node_exec_mode::async, wh::compose::node_contract::stream,
          wh::compose::node_contract::stream, async_stream_stream_sender>);
  REQUIRE_FALSE(
      wh::compose::lambda_contract_supported_v<wh::compose::node_exec_mode::async,
                                               wh::compose::node_contract::stream,
                                               wh::compose::node_contract::stream, wrong_lambda>);
}
