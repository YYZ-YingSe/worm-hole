#include <tuple>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/static_thread_scheduler.hpp"
#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/runtime/state.hpp"

namespace {

static_assert(wh::compose::result_typed_sender<
              decltype(stdexec::just(wh::core::result<wh::compose::graph_value>{})),
              wh::core::result<wh::compose::graph_value>>);
static_assert(!wh::compose::result_typed_sender<decltype(stdexec::just(wh::core::result<int>{})),
                                                wh::core::result<wh::compose::graph_value>>);
static_assert(wh::compose::graph_result_sender<
              decltype(stdexec::just(wh::core::result<wh::compose::graph_value>{}))>);
static_assert(wh::compose::graph_stream_result_sender<
              decltype(stdexec::just(wh::core::result<wh::compose::graph_stream_reader>{}))>);
static_assert(wh::compose::graph_map_result_sender<
              decltype(stdexec::just(wh::core::result<wh::compose::graph_value_map>{}))>);
static_assert(
    wh::compose::node_sync_run<decltype([](wh::compose::graph_value &, wh::core::run_context &,
                                           const wh::compose::node_runtime &)
                                            -> wh::core::result<wh::compose::graph_value> {
      return wh::compose::graph_value{1};
    })>);
static_assert(!wh::compose::node_sync_run<
              decltype([](wh::compose::graph_value &, wh::core::run_context &,
                          const wh::compose::node_runtime &) -> int { return 1; })>);
static_assert(
    wh::compose::node_async_run<decltype([](wh::compose::graph_value &, wh::core::run_context &,
                                            const wh::compose::node_runtime &) {
      return stdexec::just(wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{1}});
    })>);
static_assert(
    !wh::compose::node_async_run<decltype([](wh::compose::graph_value &, wh::core::run_context &,
                                             const wh::compose::node_runtime &) {
      return stdexec::just(wh::core::result<int>{1});
    })>);

[[nodiscard]] auto await_graph_sender(wh::compose::graph_sender sender)
    -> wh::core::result<wh::compose::graph_value> {
  auto awaited = stdexec::sync_wait(std::move(sender));
  REQUIRE(awaited.has_value());
  return std::get<0>(std::move(*awaited));
}

auto bound_nested_start(const void *state, const wh::compose::graph &, wh::core::run_context &,
                        wh::compose::graph_value &, const wh::compose::graph_call_scope *,
                        const wh::compose::node_path *, wh::compose::graph_process_state *,
                        wh::compose::detail::runtime_state::invoke_outputs *,
                        const wh::compose::graph_node_trace *) -> wh::compose::graph_sender {
  const auto value = *static_cast<const int *>(state);
  return wh::compose::detail::ready_graph_sender(
      wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{value}});
}

} // namespace

TEST_CASE("node runtime accessors and nested graph entry expose bound execution state",
          "[UT][wh/compose/node/"
          "execution.hpp][node_runtime::set_control_scheduler][condition][branch][boundary]") {
  wh::compose::node_runtime runtime{};
  wh::compose::graph_call_options options{};
  wh::compose::graph_call_scope scope{options};
  auto path = wh::compose::make_node_path({"root", "child"});
  wh::compose::graph_process_state process_state{};
  wh::compose::graph_resolved_node_observation observation{};
  wh::compose::graph_node_trace trace{};
  trace.trace_id = "trace-id";
  trace.span_id = "span-id";
  trace.parent_span_id = "parent-span";
  trace.path = &path;

  wh::testing::helper::static_thread_scheduler_helper scheduler_helper{1U};
  auto scheduler = wh::core::detail::erase_resume_scheduler(scheduler_helper.scheduler());

  runtime.set_parallel_gate(9U)
      .set_call_options(&scope)
      .set_path(&path)
      .set_control_scheduler(&scheduler)
      .set_process_state(&process_state)
      .set_observation(&observation)
      .set_trace(&trace);

  REQUIRE(runtime.parallel_gate() == 9U);
  REQUIRE(runtime.call_options() == &scope);
  REQUIRE(runtime.path() == &path);
  REQUIRE(runtime.control_scheduler() == &scheduler);
  REQUIRE(runtime.work_scheduler() == &scheduler);
  REQUIRE(runtime.process_state() == &process_state);
  REQUIRE(runtime.observation() == &observation);
  REQUIRE(runtime.trace() == &trace);

  wh::testing::helper::static_thread_scheduler_helper work_helper{1U};
  auto work_scheduler = wh::core::detail::erase_resume_scheduler(work_helper.scheduler());
  runtime.set_work_scheduler(&work_scheduler);
  REQUIRE(runtime.control_scheduler() == &scheduler);
  REQUIRE(runtime.work_scheduler() == &work_scheduler);

  wh::compose::nested_graph_entry empty{};
  REQUIRE_FALSE(empty.bound());
  wh::compose::graph dummy_graph{};
  wh::core::run_context dummy_context{};
  wh::compose::graph_value dummy_input{0};
  auto unbound = await_graph_sender(
      empty(dummy_graph, dummy_context, dummy_input, nullptr, nullptr, nullptr, nullptr, nullptr));
  REQUIRE(unbound.has_error());
  REQUIRE(unbound.error() == wh::core::errc::contract_violation);

  const int nested_value = 42;
  wh::compose::nested_graph_entry entry{
      .state = &nested_value,
      .start = &bound_nested_start,
  };
  REQUIRE(entry.bound());

  auto bound = await_graph_sender(
      entry(dummy_graph, dummy_context, dummy_input, nullptr, nullptr, nullptr, nullptr, nullptr));
  REQUIRE(bound.has_value());
  REQUIRE(*wh::core::any_cast<int>(&bound.value()) == 42);
}

TEST_CASE("execution sender helpers normalize bridge map and failure paths",
          "[UT][wh/compose/node/execution.hpp][bridge_graph_sender][condition][branch][boundary]") {
  auto mutable_capture = wh::compose::detail::make_mutable_capture(7);
  REQUIRE(mutable_capture.value == 7);

  auto unit_value = wh::compose::detail::make_graph_unit_value();
  REQUIRE(unit_value.type() == typeid(std::monostate));

  auto ready = await_graph_sender(wh::compose::detail::ready_graph_sender(
      wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{5}}));
  REQUIRE(ready.has_value());
  REQUIRE(*wh::core::any_cast<int>(&ready.value()) == 5);

  auto ready_unit = await_graph_sender(wh::compose::detail::ready_graph_unit_sender());
  REQUIRE(ready_unit.has_value());
  REQUIRE(ready_unit.value().type() == typeid(std::monostate));

  auto failed =
      await_graph_sender(wh::compose::detail::failure_graph_sender(wh::core::errc::timeout));
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::timeout);

  auto normalized_ready =
      wh::compose::detail::normalize_graph_sender(wh::compose::detail::ready_graph_sender(
          wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{17}}));
  auto normalized_ready_status = await_graph_sender(std::move(normalized_ready));
  REQUIRE(normalized_ready_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&normalized_ready_status.value()) == 17);

  auto mapped = await_graph_sender(wh::compose::detail::map_graph_sender(
      stdexec::just(wh::core::result<int>{3}),
      [](wh::core::result<int> status) -> wh::core::result<wh::compose::graph_value> {
        if (status.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(status.error());
        }
        return wh::compose::graph_value{status.value() + 2};
      }));
  REQUIRE(mapped.has_value());
  REQUIRE(*wh::core::any_cast<int>(&mapped.value()) == 5);

  auto mapped_error = await_graph_sender(wh::compose::detail::map_graph_sender(
      stdexec::just(wh::core::result<int>::failure(wh::core::errc::timeout)),
      [](wh::core::result<int> status) -> wh::core::result<wh::compose::graph_value> {
        if (status.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(status.error());
        }
        return wh::compose::graph_value{status.value()};
      }));
  REQUIRE(mapped_error.has_error());
  REQUIRE(mapped_error.error() == wh::core::errc::timeout);
}

TEST_CASE(
    "execution factory binders preserve sync behavior and enforce async scheduler contract",
    "[UT][wh/compose/node/execution.hpp][bind_node_async_factory][condition][branch][boundary]") {
  auto sync_factory = wh::compose::detail::bind_node_sync_factory(
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::node_runtime &) -> wh::core::result<wh::compose::graph_value> {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return wh::compose::graph_value{*typed + 1};
      });

  wh::compose::graph_value sync_input{8};
  wh::core::run_context context{};
  auto sync_status = sync_factory(sync_input, context, wh::compose::node_runtime{});
  REQUIRE(sync_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&sync_status.value()) == 9);

  auto async_factory = wh::compose::detail::bind_node_async_factory(
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::node_runtime &) {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return stdexec::just(
            wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{*typed + 4}});
      });

  wh::compose::graph_value async_input{2};
  auto missing_scheduler =
      await_graph_sender(async_factory(async_input, context, wh::compose::node_runtime{}));
  REQUIRE(missing_scheduler.has_error());
  REQUIRE(missing_scheduler.error() == wh::core::errc::contract_violation);

  wh::testing::helper::static_thread_scheduler_helper scheduler_helper{1U};
  auto scheduler = wh::core::detail::erase_resume_scheduler(scheduler_helper.scheduler());
  wh::compose::node_runtime runtime{};
  runtime.set_control_scheduler(&scheduler);

  auto async_status = await_graph_sender(async_factory(async_input, context, runtime));
  REQUIRE(async_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&async_status.value()) == 6);

  auto direct_sender_factory = wh::compose::detail::bind_node_async_factory(
      stdexec::just(wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{11}}));
  auto direct_status = await_graph_sender(direct_sender_factory(async_input, context, runtime));
  REQUIRE(direct_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&direct_status.value()) == 11);

  auto direct_missing_scheduler =
      await_graph_sender(direct_sender_factory(async_input, context, wh::compose::node_runtime{}));
  REQUIRE(direct_missing_scheduler.has_error());
  REQUIRE(direct_missing_scheduler.error() == wh::core::errc::contract_violation);
}
