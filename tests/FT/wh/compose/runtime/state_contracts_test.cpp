#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/invoke.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/runtime.hpp"

namespace {

using wh::testing::helper::invoke_graph_sync;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::read_graph_value;
using wh::testing::helper::test_graph_scheduler;
using wh::testing::helper::wait_sender_result;

} // namespace

TEST_CASE("compose graph enforces node state-handler metadata at runtime",
          "[core][compose][state][condition]") {
  auto build_graph = [] {
    wh::compose::graph graph{};
    auto node = wh::compose::make_passthrough_node("worker");
    node.mutable_options().state.require_pre();
    REQUIRE(graph.add_passthrough(std::move(node)).has_value());
    REQUIRE(graph.add_entry_edge("worker").has_value());
    REQUIRE(graph.add_exit_edge("worker").has_value());
    REQUIRE(graph.compile().has_value());
    return graph;
  };

  SECTION("missing registry fails with not_found") {
    auto graph = build_graph();
    wh::core::run_context context{};
    auto invoked = invoke_value_sync(graph, wh::core::any(1), context);
    REQUIRE(invoked.has_error());
    REQUIRE(invoked.error() == wh::core::errc::not_found);
  }

  SECTION("declared pre handler missing in registry fails with contract_violation") {
    auto graph = build_graph();
    wh::compose::graph_state_handler_registry handlers{};
    handlers.emplace("worker", wh::compose::graph_node_state_handlers{});
    wh::compose::graph_runtime_services services{};
    services.state_handlers = std::addressof(handlers);

    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context,
                                     wh::compose::graph_call_options{},
                                     std::addressof(services));
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() ==
            wh::core::errc::contract_violation);
  }

  SECTION("declared pre handler present succeeds") {
    auto graph = build_graph();
    wh::compose::graph_state_handler_registry handlers{};
    handlers.emplace("worker", wh::compose::graph_node_state_handlers{
                                   .pre =
                                       [](const wh::compose::graph_state_cause &,
                                          wh::compose::graph_process_state &,
                                          wh::compose::graph_value &,
                                          wh::core::run_context &)
                                           -> wh::core::result<void> {
                                         return {};
                                       },
                               });
    wh::compose::graph_runtime_services services{};
    services.state_handlers = std::addressof(handlers);

    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context,
                                     wh::compose::graph_call_options{},
                                     std::addressof(services));
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_value());
  }

  SECTION("authored pre handler succeeds without external registry") {
    wh::compose::graph graph{};
    auto node = wh::compose::make_passthrough_node("worker");
    node.mutable_options().state.bind_pre(
        [](const wh::compose::graph_state_cause &,
           wh::compose::graph_process_state &,
           wh::compose::graph_value &input,
           wh::core::run_context &) -> wh::core::result<void> {
          auto typed = read_graph_value<int>(input);
          if (typed.has_error()) {
            return wh::core::result<void>::failure(typed.error());
          }
          input = wh::core::any(typed.value() + 5);
          return {};
        });
    REQUIRE(graph.add_passthrough(std::move(node)).has_value());
    REQUIRE(graph.add_entry_edge("worker").has_value());
    REQUIRE(graph.add_exit_edge("worker").has_value());
    REQUIRE(graph.compile().has_value());

    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_value());
    auto typed = read_graph_value<int>(invoked.value().output_status.value());
    REQUIRE(typed.has_value());
    REQUIRE(typed.value() == 6);
  }
}

TEST_CASE("compose graph state handlers mutate payload and write transition log",
          "[core][compose][state][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker")).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  int observed_counter = 0;
  wh::compose::graph_state_handler_registry handlers{};
  handlers.emplace("worker", wh::compose::graph_node_state_handlers{
                                 .pre =
                                     [](const wh::compose::graph_state_cause &,
                                        wh::compose::graph_process_state &process_state,
                                        wh::compose::graph_value &input,
                                        wh::core::run_context &)
                                         -> wh::core::result<void> {
                                       auto counter = process_state.get<int>();
                                       if (counter.has_error()) {
                                         auto inserted = process_state.emplace<int>(0);
                                         if (inserted.has_error()) {
                                           return wh::core::result<void>::failure(
                                               inserted.error());
                                         }
                                         counter = inserted;
                                       }
                                       counter.value().get() += 1;
                                       auto typed = read_graph_value<int>(input);
                                       if (typed.has_error()) {
                                         return wh::core::result<void>::failure(
                                             typed.error());
                                       }
                                       input = wh::core::any(typed.value() + 1);
                                       return {};
                                     },
                                 .post =
                                     [&observed_counter](
                                         const wh::compose::graph_state_cause &,
                                         wh::compose::graph_process_state &process_state,
                                         wh::compose::graph_value &output,
                                         wh::core::run_context &)
                                         -> wh::core::result<void> {
                                       auto counter = process_state.get<int>();
                                       if (counter.has_error()) {
                                         return wh::core::result<void>::failure(
                                             counter.error());
                                       }
                                       counter.value().get() += 1;
                                       auto typed = read_graph_value<int>(output);
                                       if (typed.has_error()) {
                                         return wh::core::result<void>::failure(
                                             typed.error());
                                       }
                                       observed_counter = counter.value().get();
                                       output = wh::core::any(typed.value() * 2);
                                       return {};
                                     },
                             });
  wh::core::run_context context{};
  wh::compose::graph_runtime_services services{};
  services.state_handlers = std::addressof(handlers);
  wh::compose::graph_call_options call_options{};
  call_options.record_transition_log = true;
  auto invoked = invoke_graph_sync(graph, wh::core::any(3), context, call_options,
                                   std::addressof(services), {});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_graph_value<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 8);
  REQUIRE(observed_counter == 2);

  const auto &transitions = invoked.value().report.transition_log;
  REQUIRE(std::any_of(
      transitions.begin(), transitions.end(),
      [](const wh::compose::graph_state_transition_event &event) {
        return event.kind == wh::compose::graph_state_transition_kind::node_enter &&
               event.cause.node_key == "worker";
      }));
  REQUIRE(std::any_of(
      transitions.begin(), transitions.end(),
      [](const wh::compose::graph_state_transition_event &event) {
        return event.kind == wh::compose::graph_state_transition_kind::node_leave &&
               event.cause.node_key == "worker";
      }));
}

TEST_CASE("compose graph route commit is atomic with branch decision",
          "[core][compose][state][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("brancher",
                          [](wh::compose::graph_value &input,
                             wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            return std::move(input);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("brancher").has_value());
  REQUIRE(graph
              .add_value_branch(wh::compose::graph_value_branch{
                  .from = "brancher",
                  .end_nodes = {std::string{wh::compose::graph_end_node_key}},
                  .selector_ids =
                      wh::compose::graph_value_branch_selector_ids{
                          [](const wh::compose::graph_value &,
                             wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<std::vector<std::uint32_t>> {
                            return wh::core::result<std::vector<std::uint32_t>>::failure(
                                wh::core::errc::internal_error);
                          }},
              })
              .has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  wh::compose::graph_call_options call_options{};
  call_options.record_transition_log = true;
  auto invoked = invoke_graph_sync(graph, wh::core::any(1), context, call_options,
                                   {});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_error());
  REQUIRE(invoked.value().output_status.error() ==
          wh::core::errc::internal_error);

  const auto &transitions = invoked.value().report.transition_log;
  REQUIRE(std::any_of(
      transitions.begin(), transitions.end(),
      [](const wh::compose::graph_state_transition_event &event) {
        return event.cause.node_key == "brancher" &&
               event.kind == wh::compose::graph_state_transition_kind::node_fail;
      }));
  REQUIRE_FALSE(std::any_of(
      transitions.begin(), transitions.end(),
      [](const wh::compose::graph_state_transition_event &event) {
        return event.cause.node_key == "brancher" &&
               event.kind == wh::compose::graph_state_transition_kind::route_commit;
      }));
}

TEST_CASE("compose graph runtime binds process-state parent chain from nested invoke",
          "[core][compose][state][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker")).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_process_state parent{};
  auto inserted = parent.emplace<int>(40);
  REQUIRE(inserted.has_value());

  wh::compose::graph_state_handler_registry handlers{};
  handlers.emplace("worker", wh::compose::graph_node_state_handlers{
                                 .pre =
                                     [](const wh::compose::graph_state_cause &,
                                        wh::compose::graph_process_state &process_state,
                                        wh::compose::graph_value &input,
                                        wh::core::run_context &)
                                         -> wh::core::result<void> {
                                       auto inherited = process_state.get<int>();
                                       if (inherited.has_error()) {
                                         return wh::core::result<void>::failure(
                                             inherited.error());
                                       }
                                       auto typed = read_graph_value<int>(input);
                                       if (typed.has_error()) {
                                         return wh::core::result<void>::failure(
                                             typed.error());
                                       }
                                       input = wh::core::any(
                                           typed.value() + inherited.value().get());
                                       return {};
                                     },
                             });

  wh::compose::graph_runtime_services services{};
  services.state_handlers = std::addressof(handlers);
  wh::core::run_context context{};
  auto nested_input = wh::core::any(2);
  auto invoked = wait_sender_result<wh::core::result<wh::compose::graph_value>>(
      wh::compose::detail::start_bound_graph(
          graph, context, nested_input, nullptr, nullptr, &parent, nullptr,
          test_graph_scheduler(), nullptr, std::addressof(services)));
  REQUIRE(invoked.has_value());
  auto typed = read_graph_value<int>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 42);
}

TEST_CASE("compose graph state handlers see node-local process-state",
          "[core][compose][state][condition]") {
  bool saw_node_local_state = false;
  bool saw_parent_binding = false;
  int local_value = 0;

  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker")).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_state_handler_registry handlers{};
  handlers.emplace("worker", wh::compose::graph_node_state_handlers{
                                 .pre =
                                     [&](const wh::compose::graph_state_cause &,
                                         wh::compose::graph_process_state &process_state,
                                         wh::compose::graph_value &,
                                         wh::core::run_context &)
                                         -> wh::core::result<void> {
                                       saw_node_local_state = true;
                                       saw_parent_binding =
                                           process_state.parent() != nullptr;
                                       auto inserted = process_state.emplace<int>(9);
                                       if (inserted.has_error()) {
                                         return wh::core::result<void>::failure(
                                             inserted.error());
                                       }
                                       auto fetched = process_state.get<int>();
                                       if (fetched.has_error()) {
                                         return wh::core::result<void>::failure(
                                             fetched.error());
                                       }
                                       local_value = fetched.value().get();
                                       return {};
                                     },
                             });

  wh::compose::graph_runtime_services services{};
  services.state_handlers = std::addressof(handlers);
  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(1), context,
                                   wh::compose::graph_call_options{},
                                   std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  REQUIRE(saw_node_local_state);
  REQUIRE(saw_parent_binding);
  REQUIRE(local_value == 9);
}

TEST_CASE("compose graph stream state handlers execute at chunk granularity",
          "[core][compose][state][condition]") {
  wh::compose::graph_compile_options options{};
  options.boundary = wh::compose::graph_boundary{
      .input = wh::compose::node_contract::stream,
      .output = wh::compose::node_contract::stream,
  };
  wh::compose::graph graph{std::move(options)};
  REQUIRE(graph
              .add_passthrough(
                  wh::compose::make_passthrough_node<
                      wh::compose::node_contract::stream>("worker"))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  std::size_t chunk_count = 0U;
  wh::compose::graph_state_handler_registry handlers{};
  handlers.emplace("worker", wh::compose::graph_node_state_handlers{
                                 .stream_pre =
                                     [&chunk_count](const wh::compose::graph_state_cause &,
                                                    wh::compose::graph_process_state &,
                                                    wh::compose::graph_value &chunk_payload,
                                                    wh::core::run_context &)
                                         -> wh::core::result<void> {
                                       ++chunk_count;
                                       auto typed = read_graph_value<int>(chunk_payload);
                                       if (typed.has_error()) {
                                         return wh::core::result<void>::failure(
                                             typed.error());
                                       }
                                       chunk_payload = wh::core::any(typed.value() + 1);
                                       return {};
                                     },
                             });

  auto [writer, reader] = wh::compose::make_graph_stream();
  REQUIRE(writer.try_write(wh::core::any(1)).has_value());
  REQUIRE(writer.try_write(wh::core::any(2)).has_value());
  REQUIRE(writer.close().has_value());

  wh::compose::graph_runtime_services services{};
  services.state_handlers = std::addressof(handlers);
  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(std::move(reader)), context,
                                   wh::compose::graph_call_options{},
                                   std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  REQUIRE(chunk_count == 2U);

  auto output_stream =
      read_graph_value<wh::compose::graph_stream_reader>(
          std::move(invoked).value().output_status.value());
  REQUIRE(output_stream.has_value());
  auto output_reader = std::move(output_stream).value();
  auto first = output_reader.read();
  REQUIRE(first.has_value());
  REQUIRE_FALSE(first.value().eof);
  REQUIRE(first.value().value.has_value());
  auto first_value = read_graph_value<int>(std::move(*first.value().value));
  REQUIRE(first_value.has_value());
  REQUIRE(first_value.value() == 2);
  auto second = output_reader.read();
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second.value().eof);
  REQUIRE(second.value().value.has_value());
  auto second_value = read_graph_value<int>(std::move(*second.value().value));
  REQUIRE(second_value.has_value());
  REQUIRE(second_value.value() == 3);
}

TEST_CASE(
    "compose graph value state handlers observe stream payload handles when stream handlers are absent",
    "[core][compose][state][condition]") {
  auto build_graph = [](const bool enable_pre, const bool enable_post) {
    wh::compose::graph_compile_options options{};
    options.boundary = wh::compose::graph_boundary{
        .input = wh::compose::node_contract::stream,
        .output = wh::compose::node_contract::stream,
    };
    wh::compose::graph graph{std::move(options)};
    auto worker =
        wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
            "worker");
    if (enable_pre) {
      worker.mutable_options().state.require_pre();
    }
    if (enable_post) {
      worker.mutable_options().state.require_post();
    }
    REQUIRE(graph.add_passthrough(std::move(worker)).has_value());
    REQUIRE(graph.add_entry_edge("worker").has_value());
    REQUIRE(graph.add_exit_edge("worker").has_value());
    REQUIRE(graph.compile().has_value());
    return graph;
  };

  auto build_stream_input = [] {
    auto [writer, reader] = wh::compose::make_graph_stream();
    REQUIRE(writer.try_write(wh::core::any(1)).has_value());
    REQUIRE(writer.try_write(wh::core::any(2)).has_value());
    REQUIRE(writer.close().has_value());
    return wh::core::any(std::move(reader));
  };

  SECTION("pre-handler receives reader handle") {
    auto graph = build_graph(true, false);
    bool saw_reader_handle = false;
    wh::compose::graph_state_handler_registry handlers{};
    handlers.emplace("worker", wh::compose::graph_node_state_handlers{
                                   .pre =
                                       [&saw_reader_handle](
                                           const wh::compose::graph_state_cause &,
                                           wh::compose::graph_process_state &,
                                           wh::compose::graph_value &input,
                                           wh::core::run_context &)
                                           -> wh::core::result<void> {
                                         saw_reader_handle =
                                             wh::core::any_cast<
                                                 wh::compose::graph_stream_reader>(
                                                 &input) != nullptr;
                                         return {};
                                       },
                               });

    wh::compose::graph_runtime_services services{};
    services.state_handlers = std::addressof(handlers);
    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, build_stream_input(), context,
                                     wh::compose::graph_call_options{},
                                     std::addressof(services));
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_value());
    REQUIRE(saw_reader_handle);
  }

  SECTION("post-handler receives reader handle") {
    auto graph = build_graph(false, true);
    bool saw_reader_handle = false;
    wh::compose::graph_state_handler_registry handlers{};
    handlers.emplace("worker", wh::compose::graph_node_state_handlers{
                                   .post =
                                       [&saw_reader_handle](
                                           const wh::compose::graph_state_cause &,
                                           wh::compose::graph_process_state &,
                                           wh::compose::graph_value &output,
                                           wh::core::run_context &)
                                           -> wh::core::result<void> {
                                         saw_reader_handle =
                                             wh::core::any_cast<
                                                 wh::compose::graph_stream_reader>(
                                                 &output) != nullptr;
                                         return {};
                                       },
                               });

    wh::compose::graph_runtime_services services{};
    services.state_handlers = std::addressof(handlers);
    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, build_stream_input(), context,
                                     wh::compose::graph_call_options{},
                                     std::addressof(services));
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_value());
    REQUIRE(saw_reader_handle);
  }
}

TEST_CASE("compose graph compile rejects typed state bindings without exact gate",
          "[core][compose][state][boundary]") {
  SECTION("typed value pre hook on open value input fails at graph compile") {
    wh::compose::graph graph{};
    auto node = wh::compose::make_passthrough_node("worker");
    node.mutable_options().state.bind_pre<int>(
        [](const wh::compose::graph_state_cause &,
           wh::compose::graph_process_state &,
           int &,
           wh::core::run_context &) -> wh::core::result<void> { return {}; });
    REQUIRE(graph.add_passthrough(std::move(node)).has_value());
    REQUIRE(graph.add_entry_edge("worker").has_value());
    REQUIRE(graph.add_exit_edge("worker").has_value());

    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
  }

  SECTION("typed stream chunk hook fails until chunk type is compile-visible") {
    wh::compose::graph_compile_options options{};
    options.boundary = wh::compose::graph_boundary{
        .input = wh::compose::node_contract::stream,
        .output = wh::compose::node_contract::stream,
    };
    wh::compose::graph graph{std::move(options)};
    auto node =
        wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
            "worker");
    node.mutable_options().state.bind_stream_pre<int>(
        [](const wh::compose::graph_state_cause &,
           wh::compose::graph_process_state &,
           int &,
           wh::core::run_context &) -> wh::core::result<void> { return {}; });
    REQUIRE(graph.add_passthrough(std::move(node)).has_value());
    REQUIRE(graph.add_entry_edge("worker").has_value());
    REQUIRE(graph.add_exit_edge("worker").has_value());

    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
  }
}

TEST_CASE("compose custom component may access node runtime process-state",
          "[core][compose][component][state][condition]") {
  struct runtime_component {
    auto invoke(int input, wh::core::run_context &,
                const wh::compose::node_runtime &runtime) const
        -> wh::core::result<int> {
      auto counter = wh::compose::node_process_state_ref<int>(runtime);
      if (counter.has_error()) {
        auto inserted = wh::compose::emplace_node_process_state<int>(runtime, 0);
        if (inserted.has_error()) {
          return wh::core::result<int>::failure(inserted.error());
        }
        counter = inserted;
      }
      counter.value().get() += 1;
      return input + counter.value().get();
    }
  };

  wh::compose::graph graph{};
  auto node = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, int, int>("worker",
                                                   runtime_component{});
  REQUIRE(graph.add_component(std::move(node)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_graph_value<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 2);
}

TEST_CASE("compose graph compile enforces state generation gate for state handlers",
          "[core][compose][state][boundary]") {
  wh::compose::graph_compile_options options{};
  options.enable_local_state_generation = false;
  wh::compose::graph graph{std::move(options)};

  auto worker = wh::compose::make_passthrough_node("worker");
  worker.mutable_options().state.require_pre();
  REQUIRE(graph.add_passthrough(std::move(worker)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());

  auto compiled = graph.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::contract_violation);
}
