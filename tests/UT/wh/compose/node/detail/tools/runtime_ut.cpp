#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/node/detail/tools/runtime.hpp"

namespace {

[[nodiscard]] auto make_batch(std::initializer_list<wh::compose::tool_call> calls)
    -> wh::compose::graph_value {
  return wh::compose::graph_value{
      wh::compose::tool_batch{.calls = std::vector<wh::compose::tool_call>{calls}}};
}

[[nodiscard]] auto await_graph_sender(wh::compose::graph_sender sender)
    -> wh::core::result<wh::compose::graph_value> {
  auto waited = stdexec::sync_wait(std::move(sender));
  REQUIRE(waited.has_value());
  return std::get<0>(std::move(*waited));
}

[[nodiscard]] auto collect_tool_results(wh::compose::graph_value &value)
    -> std::vector<wh::compose::tool_result> * {
  return wh::core::any_cast<std::vector<wh::compose::tool_result>>(&value);
}

[[nodiscard]] auto collect_tool_events(wh::compose::graph_value &value)
    -> wh::core::result<std::vector<wh::compose::tool_event>> {
  auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(&value);
  if (reader == nullptr) {
    return wh::core::result<std::vector<wh::compose::tool_event>>::failure(
        wh::core::errc::type_mismatch);
  }
  auto collected = wh::compose::collect_graph_stream_reader(std::move(*reader));
  if (collected.has_error()) {
    return wh::core::result<std::vector<wh::compose::tool_event>>::failure(
        collected.error());
  }
  std::vector<wh::compose::tool_event> events{};
  events.reserve(collected.value().size());
  for (auto &entry : collected.value()) {
    auto *event = wh::core::any_cast<wh::compose::tool_event>(&entry);
    REQUIRE(event != nullptr);
    events.push_back(*event);
  }
  return events;
}

} // namespace

TEST_CASE("tools runtime sync covers invalid input sequential guard value output and stream output paths",
          "[UT][wh/compose/node/detail/tools/runtime.hpp][run_tools_sync][condition][branch][boundary]") {
  wh::compose::tool_registry registry{};
  registry.emplace(
      "echo",
      wh::compose::tool_entry{
          .invoke =
              [](const wh::compose::tool_call &call, wh::tool::call_scope)
                  -> wh::core::result<wh::compose::graph_value> {
                return wh::compose::graph_value{call.arguments};
              },
          .stream =
              [](const wh::compose::tool_call &call, wh::tool::call_scope)
                  -> wh::core::result<wh::compose::graph_stream_reader> {
                return wh::compose::make_values_stream_reader(
                    std::vector<wh::compose::graph_value>{
                        wh::compose::graph_value{call.arguments}});
              },
      });

  wh::compose::tools_options options{};
  options.middleware.push_back({
      .after =
          [](const wh::compose::tool_call &, wh::compose::graph_value &value,
             const wh::tool::call_scope &) -> wh::core::result<void> {
        if (auto *typed = wh::core::any_cast<std::string>(&value);
            typed != nullptr) {
          value = wh::compose::graph_value{*typed + "-after"};
        }
        return {};
      },
  });

  wh::core::run_context context{};
  wh::compose::node_runtime runtime{};

  auto type_mismatch = wh::compose::detail::run_tools_sync<
      wh::compose::node_contract::value>(wh::compose::graph_value{1}, context,
                                         runtime, registry, options);
  REQUIRE(type_mismatch.has_error());
  REQUIRE(type_mismatch.error() == wh::core::errc::type_mismatch);

  options.sequential = false;
  auto not_supported = wh::compose::detail::run_tools_sync<
      wh::compose::node_contract::value>(
      make_batch({{.call_id = "a", .tool_name = "echo", .arguments = "x"}}),
      context, runtime, registry, options);
  REQUIRE(not_supported.has_error());
  REQUIRE(not_supported.error() == wh::core::errc::not_supported);

  options.sequential = true;
  auto value_status = wh::compose::detail::run_tools_sync<
      wh::compose::node_contract::value>(
      make_batch({{.call_id = "a", .tool_name = "echo", .arguments = "x"},
                  {.call_id = "b", .tool_name = "echo", .arguments = "y"}}),
      context, runtime, registry, options);
  REQUIRE(value_status.has_value());
  auto *results = collect_tool_results(value_status.value());
  REQUIRE(results != nullptr);
  REQUIRE(results->size() == 2U);
  REQUIRE((*results)[0].call_id == "a");
  REQUIRE(*wh::core::any_cast<std::string>(&(*results)[0].value) == "x-after");
  REQUIRE((*results)[1].call_id == "b");
  REQUIRE(*wh::core::any_cast<std::string>(&(*results)[1].value) == "y-after");

  auto stream_status = wh::compose::detail::run_tools_sync<
      wh::compose::node_contract::stream>(
      make_batch({{.call_id = "stream", .tool_name = "echo", .arguments = "z"}}),
      context, runtime, registry, options);
  REQUIRE(stream_status.has_value());
  auto events = collect_tool_events(stream_status.value());
  REQUIRE(events.has_value());
  REQUIRE(events.value().size() == 1U);
  REQUIRE(events.value().front().call_id == "stream");
  REQUIRE(events.value().front().tool_name == "echo");
  REQUIRE(*wh::core::any_cast<std::string>(&events.value().front().value) ==
          "z-after");
}

TEST_CASE("tools runtime async covers immediate error paths and parallel gate fanout for value and stream outputs",
          "[UT][wh/compose/node/detail/tools/runtime.hpp][run_tools_async][concurrency][branch]") {
  exec::static_thread_pool pool{2};
  std::atomic<int> active{0};
  std::atomic<int> peak{0};

  wh::compose::tool_registry registry{};
  registry.emplace(
      "echo",
      wh::compose::tool_entry{
          .async_invoke =
              [&](wh::compose::tool_call call,
                  wh::tool::call_scope) -> wh::compose::tools_invoke_sender {
            return stdexec::schedule(pool.get_scheduler()) |
                   stdexec::then([call = std::move(call), &active, &peak] {
                     const auto now =
                         active.fetch_add(1, std::memory_order_acq_rel) + 1;
                     peak.store(std::max(peak.load(std::memory_order_relaxed), now),
                                std::memory_order_release);
                     std::this_thread::sleep_for(std::chrono::milliseconds(10));
                     active.fetch_sub(1, std::memory_order_acq_rel);
                     return wh::core::result<wh::compose::graph_value>{
                         wh::compose::graph_value{call.arguments}};
                   });
          },
          .async_stream =
              [&](wh::compose::tool_call call,
                  wh::tool::call_scope) -> wh::compose::tools_stream_sender {
            return stdexec::schedule(pool.get_scheduler()) |
                   stdexec::then([call = std::move(call)] {
                     return wh::compose::make_values_stream_reader(
                         std::vector<wh::compose::graph_value>{
                             wh::compose::graph_value{call.arguments}});
                   });
          },
      });

  wh::compose::tools_options options{};
  options.sequential = false;
  wh::core::run_context context{};
  wh::compose::node_runtime runtime{};

  auto immediate_error = await_graph_sender(wh::compose::detail::run_tools_async<
      wh::compose::node_contract::value>(wh::compose::graph_value{1}, context,
                                         runtime, registry, options));
  REQUIRE(immediate_error.has_error());
  REQUIRE(immediate_error.error() == wh::core::errc::type_mismatch);

  runtime.set_parallel_gate(1U);
  auto serial_value = await_graph_sender(wh::compose::detail::run_tools_async<
      wh::compose::node_contract::value>(
      make_batch({{.call_id = "a", .tool_name = "echo", .arguments = "1"},
                  {.call_id = "b", .tool_name = "echo", .arguments = "2"},
                  {.call_id = "c", .tool_name = "echo", .arguments = "3"}}),
      context, runtime, registry, options));
  REQUIRE(serial_value.has_value());
  auto *serial_results = collect_tool_results(serial_value.value());
  REQUIRE(serial_results != nullptr);
  REQUIRE(serial_results->size() == 3U);
  REQUIRE(peak.load(std::memory_order_acquire) == 1);

  active.store(0, std::memory_order_release);
  peak.store(0, std::memory_order_release);
  runtime.set_parallel_gate(2U);
  auto parallel_value = await_graph_sender(wh::compose::detail::run_tools_async<
      wh::compose::node_contract::value>(
      make_batch({{.call_id = "a", .tool_name = "echo", .arguments = "1"},
                  {.call_id = "b", .tool_name = "echo", .arguments = "2"},
                  {.call_id = "c", .tool_name = "echo", .arguments = "3"}}),
      context, runtime, registry, options));
  REQUIRE(parallel_value.has_value());
  REQUIRE(peak.load(std::memory_order_acquire) >= 2);

  auto stream_value = await_graph_sender(wh::compose::detail::run_tools_async<
      wh::compose::node_contract::stream>(
      make_batch({{.call_id = "stream", .tool_name = "echo", .arguments = "chunk"}}),
      context, runtime, registry, options));
  REQUIRE(stream_value.has_value());
  auto events = collect_tool_events(stream_value.value());
  REQUIRE(events.has_value());
  REQUIRE(events.value().size() == 1U);
  REQUIRE(events.value().front().call_id == "stream");
  REQUIRE(*wh::core::any_cast<std::string>(&events.value().front().value) ==
          "chunk");
}
