#include <atomic>
#include <chrono>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/authored.hpp"
#include "wh/core/stdexec/result_sender.hpp"

namespace {

using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_dual_scheduler_env;
using wh::testing::helper::make_graph_request;

template <typename value_t>
[[nodiscard]] auto any_get(const wh::core::any &value) noexcept -> const value_t * {
  return wh::core::any_cast<value_t>(&value);
}

} // namespace

TEST_CASE("compose branch supports stream-condition routing",
          "[core][compose][branch][condition]") {
  wh::compose::graph graph{wh::compose::graph_boundary{
      .input = wh::compose::node_contract::stream,
      .output = wh::compose::node_contract::stream,
  }};
  auto route = wh::compose::make_passthrough_node<wh::compose::node_contract::stream>("route");
  auto stream_path =
      wh::compose::make_passthrough_node<wh::compose::node_contract::stream>("stream-path");
  REQUIRE(graph.add_passthrough(std::move(route)).has_value());
  REQUIRE(graph.add_passthrough(std::move(stream_path)).has_value());
  REQUIRE(graph.add_entry_edge("route").has_value());
  REQUIRE(graph.add_exit_edge("stream-path").has_value());

  wh::compose::stream_branch branch{};
  REQUIRE(branch.add_target("stream-path").has_value());
  REQUIRE(
      branch
          .set_selector([](wh::compose::graph_stream_reader,
                           wh::core::run_context &) -> wh::compose::stream_branch_key_sender {
            return wh::compose::stream_branch_key_sender{
                wh::core::detail::ready_sender(wh::core::result<std::vector<std::string>>{
                    std::vector<std::string>{"stream-path"}})};
          })
          .has_value());
  REQUIRE(branch.apply(graph, "route").has_value());
  REQUIRE(graph.compile().has_value());

  auto stream_reader = wh::compose::make_single_value_stream_reader(wh::core::any(3));
  REQUIRE(stream_reader.has_value());
  wh::core::run_context context{};
  auto output = invoke_value_sync(graph, std::move(stream_reader).value(), context);
  REQUIRE(output.has_value());
  const auto *typed = any_get<wh::compose::graph_stream_reader>(output.value());
  REQUIRE(typed != nullptr);
}

TEST_CASE("compose branch supports async stream-condition routing",
          "[core][compose][branch][condition][async]") {
  wh::compose::graph graph{wh::compose::graph_boundary{
      .input = wh::compose::node_contract::stream,
      .output = wh::compose::node_contract::stream,
  }};
  auto route = wh::compose::make_passthrough_node<wh::compose::node_contract::stream>("route");
  auto stream_path =
      wh::compose::make_passthrough_node<wh::compose::node_contract::stream>("stream-path");
  REQUIRE(graph.add_passthrough(std::move(route)).has_value());
  REQUIRE(graph.add_passthrough(std::move(stream_path)).has_value());
  REQUIRE(graph.add_entry_edge("route").has_value());
  REQUIRE(graph.add_exit_edge("stream-path").has_value());

  exec::static_thread_pool selector_pool{1U};
  std::atomic<bool> selector_ran{false};

  wh::compose::stream_branch branch{};
  REQUIRE(branch.add_target("stream-path").has_value());
  REQUIRE(
      branch
          .set_selector([&](wh::compose::graph_stream_reader,
                            wh::core::run_context &) -> wh::compose::stream_branch_key_sender {
            return wh::compose::stream_branch_key_sender{
                stdexec::starts_on(selector_pool.get_scheduler(),
                                   stdexec::just() | stdexec::then([&]() {
                                     selector_ran.store(true, std::memory_order_release);
                                     return wh::core::result<std::vector<std::string>>{
                                         std::vector<std::string>{"stream-path"}};
                                   }))};
          })
          .has_value());
  REQUIRE(branch.apply(graph, "route").has_value());
  REQUIRE(graph.compile().has_value());

  auto stream_reader = wh::compose::make_single_value_stream_reader(wh::core::any(7));
  REQUIRE(stream_reader.has_value());

  wh::core::run_context context{};
  using status_t = wh::core::result<wh::compose::graph_invoke_result>;
  exec::static_thread_pool launch_pool{1U};
  auto env = make_dual_scheduler_env(launch_pool.get_scheduler(), launch_pool.get_scheduler());
  wh::testing::helper::sender_capture<status_t> capture{};
  auto op = stdexec::connect(
      graph.invoke(context, make_graph_request(std::move(stream_reader).value())),
      wh::testing::helper::sender_capture_receiver<status_t, decltype(env)>{&capture, env});
  stdexec::start(op);

  REQUIRE(capture.ready.try_acquire_for(std::chrono::milliseconds{500}));
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(selector_ran.load(std::memory_order_acquire));
  auto status = std::move(*capture.value);
  REQUIRE(status.has_value());
  REQUIRE(status->output_status.has_value());

  auto *typed = wh::core::any_cast<wh::compose::graph_stream_reader>(&status->output_status.value());
  REQUIRE(typed != nullptr);
  auto collected = wh::compose::collect_graph_stream_reader(std::move(*typed));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);
  auto *payload = wh::core::any_cast<int>(&collected.value().front());
  REQUIRE(payload != nullptr);
  REQUIRE(*payload == 7);
}
