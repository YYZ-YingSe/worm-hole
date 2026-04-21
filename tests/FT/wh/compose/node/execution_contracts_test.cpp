#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/node.hpp"

namespace {

using wh::testing::helper::read_graph_value;

} // namespace

TEST_CASE("graph sender type erasure should preserve move-only stream payload",
          "[core][compose][async][stream][diagnostic]") {
  auto direct_reader = wh::compose::make_single_value_stream_reader(wh::core::any(7));
  REQUIRE(direct_reader.has_value());
  auto direct_waited = stdexec::sync_wait(stdexec::just(
      wh::core::result<wh::compose::graph_value>{wh::core::any(std::move(direct_reader).value())}));
  REQUIRE(direct_waited.has_value());
  auto direct_status = std::get<0>(std::move(direct_waited).value());
  REQUIRE(direct_status.has_value());
  auto direct_payload =
      read_graph_value<wh::compose::graph_stream_reader>(std::move(direct_status).value());
  REQUIRE(direct_payload.has_value());

  auto erased_reader = wh::compose::make_single_value_stream_reader(wh::core::any(9));
  REQUIRE(erased_reader.has_value());
  auto erased_sender = wh::compose::detail::bridge_graph_sender(stdexec::just(
      wh::core::result<wh::compose::graph_value>{wh::core::any(std::move(erased_reader).value())}));
  auto erased_waited = stdexec::sync_wait(std::move(erased_sender));
  REQUIRE(erased_waited.has_value());
  auto erased_status = std::get<0>(std::move(erased_waited).value());
  REQUIRE(erased_status.has_value());
  INFO(erased_status.value().info().name);
  auto erased_payload =
      read_graph_value<wh::compose::graph_stream_reader>(std::move(erased_status).value());
  REQUIRE(erased_payload.has_value());

  exec::static_thread_pool pool{1U};
  auto pool_scheduler = pool.get_scheduler();
  const auto &pool_scheduler_ref = pool_scheduler;
  auto scheduled_reader = wh::compose::make_single_value_stream_reader(wh::core::any(11));
  REQUIRE(scheduled_reader.has_value());
  auto scheduled_sender =
      wh::compose::detail::bridge_graph_sender(wh::core::detail::write_sender_scheduler(
          stdexec::just(wh::core::result<wh::compose::graph_value>{
              wh::core::any(std::move(scheduled_reader).value())}),
          pool_scheduler_ref));
  auto scheduled_waited = stdexec::sync_wait(std::move(scheduled_sender));
  REQUIRE(scheduled_waited.has_value());
  auto scheduled_status = std::get<0>(std::move(scheduled_waited).value());
  REQUIRE(scheduled_status.has_value());
  INFO(scheduled_status.value().info().name);
  auto scheduled_payload =
      read_graph_value<wh::compose::graph_stream_reader>(std::move(scheduled_status).value());
  REQUIRE(scheduled_payload.has_value());
}
