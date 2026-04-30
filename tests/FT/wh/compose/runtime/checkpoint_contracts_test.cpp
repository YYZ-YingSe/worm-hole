#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/runtime/checkpoint.hpp"

namespace {

using wh::testing::helper::read_graph_value;
using wh::testing::helper::read_graph_value_cref;

} // namespace

TEST_CASE("compose checkpoint default stream codec round-trips stream payloads",
          "[core][compose][runtime][checkpoint][functional]") {
  auto default_codec = wh::compose::make_default_stream_codec();
  REQUIRE(static_cast<bool>(default_codec.to_value));
  REQUIRE(static_cast<bool>(default_codec.to_stream));

  wh::core::run_context context{};
  auto source_stream = wh::compose::make_values_stream_reader(
      std::vector<wh::compose::graph_value>{wh::core::any(3), wh::core::any(4)});
  REQUIRE(source_stream.has_value());

  auto encoded_waited =
      stdexec::sync_wait(default_codec.to_value(std::move(source_stream).value(), context));
  REQUIRE(encoded_waited.has_value());
  auto encoded = std::get<0>(std::move(*encoded_waited));
  REQUIRE(encoded.has_value());
  auto encoded_chunks =
      read_graph_value_cref<std::vector<wh::compose::graph_value>>(encoded.value());
  REQUIRE(encoded_chunks.has_value());
  REQUIRE(encoded_chunks.value().get().size() == 2U);
  REQUIRE(read_graph_value<int>(encoded_chunks.value().get()[0]).value() == 3);
  REQUIRE(read_graph_value<int>(encoded_chunks.value().get()[1]).value() == 4);

  auto decoded = default_codec.to_stream(std::move(encoded).value(), context);
  REQUIRE(decoded.has_value());
  auto collected = wh::compose::collect_graph_stream_reader(std::move(decoded).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 2U);
  REQUIRE(read_graph_value<int>(collected.value()[0]).value() == 3);
  REQUIRE(read_graph_value<int>(collected.value()[1]).value() == 4);

  auto single_decoded = default_codec.to_stream(wh::core::any(17), context);
  REQUIRE(single_decoded.has_value());
  auto single_collected =
      wh::compose::collect_graph_stream_reader(std::move(single_decoded).value());
  REQUIRE(single_collected.has_value());
  REQUIRE(single_collected.value().size() == 1U);
  REQUIRE(read_graph_value<int>(single_collected.value().front()).value() == 17);
}
