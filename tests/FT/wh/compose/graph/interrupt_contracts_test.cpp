#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/start.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/any.hpp"

namespace {

template <typename value_t>
[[nodiscard]] auto read_any(const wh::core::any &value) -> wh::core::result<value_t> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    if constexpr (std::copy_constructible<value_t>) {
      return *typed;
    } else {
      return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
    }
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] auto read_any(wh::core::any &&value) -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto inline_graph_scheduler() noexcept
    -> const wh::core::detail::any_resume_scheduler_t & {
  static const auto scheduler =
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});
  return scheduler;
}

using wh::testing::helper::checkpoint_entry_input;
using wh::testing::helper::find_checkpoint_entry_input;
using wh::testing::helper::find_checkpoint_node_input;
using wh::testing::helper::invoke_graph_sync;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_auto_contract_edge_options;
using wh::testing::helper::make_int_graph_stream;
using wh::testing::helper::make_tool_batch;
using wh::testing::helper::wait_sender_result;

} // namespace

TEST_CASE("compose graph resume matching respects runtime node-path prefix",
          "[core][compose][interrupt][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_lambda(wh::testing::helper::make_int_add_node("worker", 1)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  context.resume_info.emplace();
  REQUIRE(wh::compose::add_resume_target(*context.resume_info, "ctx-worker",
                                         wh::core::make_address({"graph", "parent", "worker"}),
                                         wh::core::any{1})
              .has_value());
  auto path_prefix = wh::compose::make_node_path({"parent"});
  auto nested_input = wh::core::any(4);
  auto invoked = wait_sender_result<wh::core::result<wh::compose::graph_value>>(
      wh::compose::detail::start_bound_graph(graph, context, nested_input, nullptr, &path_prefix,
                                             nullptr, nullptr, inline_graph_scheduler(),
                                             inline_graph_scheduler()));
  REQUIRE(invoked.has_value());
  auto typed = read_any<int>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 5);
  REQUIRE_FALSE(context.interrupt_info.has_value());
}

TEST_CASE("compose graph runtime does not fall back to legacy interrupt payload",
          "[core][compose][interrupt][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("inc",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("inc").has_value());
  REQUIRE(graph.add_exit_edge("inc").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_state checkpoint{};
  checkpoint.checkpoint_id = "missing-rerun";
  checkpoint.restore_shape = graph.restore_shape();
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "missing-rerun";
  REQUIRE(store.save(checkpoint, write_options).has_value());

  wh::core::run_context context{};
  wh::compose::checkpoint_load_options load_options{};
  load_options.checkpoint_id = "missing-rerun";
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.load = load_options;
  context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "legacy-input",
      .location = wh::core::make_address({"graph", "inc"}),
      .state = wh::core::any{10},
  };

  auto invoked = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(), context,
                                   controls, std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_error());
  REQUIRE(invoked.value().output_status.error() == wh::core::errc::contract_violation);
}

TEST_CASE("compose graph interrupt hooks can cancel at pre and post node boundary",
          "[core][compose][interrupt][condition]") {
  auto build_graph = [] {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda(
                    "worker",
                    [](wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
                .has_value());
    REQUIRE(graph.add_entry_edge("worker").has_value());
    REQUIRE(graph.add_exit_edge("worker").has_value());
    REQUIRE(graph.compile().has_value());
    return graph;
  };

  SECTION("pre hook cancels before node invoke") {
    auto graph = build_graph();
    wh::compose::graph_invoke_controls controls{};
    controls.interrupt.pre_hook = wh::compose::graph_interrupt_node_hook{
        [](const std::string_view node_key, const wh::compose::graph_value &,
           wh::core::run_context &) -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
          if (node_key != "worker") {
            return std::optional<wh::core::interrupt_signal>{};
          }
          return std::optional<wh::core::interrupt_signal>{wh::compose::make_interrupt_signal(
              "pre-int", wh::core::make_address({"graph", "worker"}), std::string{"stop"})};
        }};
    wh::core::run_context context{};
    auto status = invoke_graph_sync(graph, wh::core::any(1), context, controls);
    REQUIRE(status.has_value());
    REQUIRE(status.value().output_status.has_error());
    REQUIRE(status.value().output_status.error() == wh::core::errc::canceled);
    REQUIRE(context.interrupt_info.has_value());
    REQUIRE(context.interrupt_info->interrupt_id == "pre-int");
  }

  SECTION("post hook cancels after node invoke") {
    auto graph = build_graph();
    wh::compose::graph_invoke_controls controls{};
    controls.interrupt.post_hook = wh::compose::graph_interrupt_node_hook{
        [](const std::string_view node_key, const wh::compose::graph_value &payload,
           wh::core::run_context &) -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
          if (node_key != "worker") {
            return std::optional<wh::core::interrupt_signal>{};
          }
          return std::optional<wh::core::interrupt_signal>{wh::compose::make_interrupt_signal(
              "post-int", wh::core::make_address({"graph", "worker"}), payload)};
        }};
    wh::core::run_context context{};
    auto status = invoke_graph_sync(graph, wh::core::any(1), context, controls);
    REQUIRE(status.has_value());
    REQUIRE(status.value().output_status.has_error());
    REQUIRE(status.value().output_status.error() == wh::core::errc::canceled);
    REQUIRE(context.interrupt_info.has_value());
    REQUIRE(context.interrupt_info->interrupt_id == "post-int");
  }
}

TEST_CASE("compose graph runtime applies resume decision and batch payload sessions",
          "[core][compose][interrupt][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](const wh::compose::graph_value &, wh::core::run_context &context,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    if (!context.resume_info.has_value()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          wh::core::errc::not_found);
                    }
                    auto approved = wh::compose::consume_resume_data<wh::compose::resume_patch>(
                        *context.resume_info, "ctx-a");
                    if (approved.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(approved.error());
                    }
                    auto edited = wh::compose::consume_resume_data<wh::compose::resume_patch>(
                        *context.resume_info, "ctx-b");
                    if (edited.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(edited.error());
                    }
                    auto approved_value = wh::core::any_cast<int>(&approved.value().data);
                    auto edited_value = wh::core::any_cast<int>(&edited.value().data);
                    if (approved_value == nullptr || edited_value == nullptr) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          wh::core::errc::type_mismatch);
                    }
                    return wh::core::any(*approved_value + *edited_value);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  const auto root = wh::core::make_address({"graph"});
  const std::vector<wh::core::interrupt_context> contexts{
      wh::core::interrupt_context{
          .interrupt_id = "ctx-a",
          .location = root.append("worker"),
          .state = wh::core::any{3},
      },
      wh::core::interrupt_context{
          .interrupt_id = "ctx-b",
          .location = root.append("worker"),
          .state = wh::core::any{7},
      },
  };
  wh::compose::interrupt_resume_decision decision{};
  decision.interrupt_context_id = "ctx-a";
  decision.decision = wh::compose::interrupt_decision_kind::approve;
  std::vector<wh::compose::resume_batch_item> batch{
      {.interrupt_context_id = "ctx-b", .data = wh::core::any{11}},
  };

  wh::compose::graph_invoke_controls controls{};
  controls.resume.contexts = contexts;
  controls.resume.decision = decision;
  controls.resume.batch_items = batch;
  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(std::monostate{}), context, controls);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 14);
}

TEST_CASE("compose graph runtime ingests subgraph interrupt payload sources",
          "[core][compose][interrupt][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("worker",
                          [](const wh::compose::graph_value &, wh::core::run_context &context,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            if (!context.resume_info.has_value()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  wh::core::errc::not_found);
                            }
                            auto sub = wh::compose::consume_resume_data<wh::compose::resume_patch>(
                                *context.resume_info, "ctx-sub");
                            if (sub.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  sub.error());
                            }
                            auto sub_value = wh::core::any_cast<int>(&sub.value().data);
                            if (sub_value == nullptr) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  wh::core::errc::type_mismatch);
                            }
                            return wh::core::any(*sub_value);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  const auto location = wh::core::make_address({"graph", "worker"});
  wh::compose::graph_invoke_controls controls{};
  controls.interrupt.subgraph_signals = std::vector<wh::core::interrupt_signal>{
      wh::compose::make_interrupt_signal("ctx-sub", location, 5)};
  controls.resume.batch_items = std::vector<wh::compose::resume_batch_item>{
      {.interrupt_context_id = "ctx-sub", .data = wh::core::any{7}},
  };
  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(std::monostate{}), context, controls);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 7);
}

TEST_CASE("compose graph internal interrupt persist follows frozen policy",
          "[core][compose][interrupt][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "interrupt-policy";

  auto invoke_with_policy =
      [&](const bool manual_persist) -> wh::core::result<wh::compose::graph_value> {
    wh::compose::graph_runtime_services services{};
    services.checkpoint.store = std::addressof(store);
    wh::compose::graph_invoke_controls controls{};
    controls.checkpoint.save = write_options;
    controls.interrupt.pre_hook = wh::compose::graph_interrupt_node_hook{
        [](const std::string_view node_key, const wh::compose::graph_value &,
           wh::core::run_context &) -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
          if (node_key != "worker") {
            return std::optional<wh::core::interrupt_signal>{};
          }
          return std::optional<wh::core::interrupt_signal>{wh::compose::make_interrupt_signal(
              "policy-int", wh::core::make_address({"graph", "worker"}), std::monostate{})};
        }};
    wh::core::run_context context{};
    wh::compose::graph_call_options options{};
    options.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
        .timeout = std::nullopt,
        .mode = wh::compose::graph_interrupt_timeout_mode::wait_inflight,
        .auto_persist_external_interrupt = true,
        .manual_persist_internal_interrupt = manual_persist,
    };
    auto status = invoke_graph_sync(graph, wh::core::any(1), context, options,
                                    std::addressof(services), controls);
    if (status.has_error()) {
      return wh::core::result<wh::compose::graph_value>::failure(status.error());
    }
    return std::move(status).value().output_status;
  };

  auto skipped_persist = invoke_with_policy(false);
  REQUIRE(skipped_persist.has_error());
  REQUIRE(skipped_persist.error() == wh::core::errc::canceled);
  wh::compose::checkpoint_load_options load_options{};
  load_options.checkpoint_id = "interrupt-policy";
  auto missing = store.load(load_options);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  auto enabled_persist = invoke_with_policy(true);
  REQUIRE(enabled_persist.has_error());
  REQUIRE(enabled_persist.error() == wh::core::errc::canceled);
  auto persisted = store.load(load_options);
  REQUIRE(persisted.has_value());
}

TEST_CASE("compose graph external interrupt publishes resolution mode",
          "[core][compose][interrupt][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto run_with_options = [&](wh::compose::graph_call_options call_options)
      -> wh::core::result<wh::compose::graph_external_interrupt_resolution_kind> {
    wh::core::run_context context{};
    context.interrupt_info = wh::core::interrupt_context{
        .interrupt_id = "external-int",
        .location = wh::core::make_address({"graph", "worker"}),
        .state = wh::core::any{std::monostate{}},
    };
    auto status =
        invoke_graph_sync(graph, wh::core::any(1), context, std::move(call_options), nullptr, {});
    if (status.has_error()) {
      return wh::core::result<wh::compose::graph_external_interrupt_resolution_kind>::failure(
          status.error());
    }
    if (status.value().output_status.has_value()) {
      return wh::core::result<wh::compose::graph_external_interrupt_resolution_kind>::failure(
          wh::core::errc::contract_violation);
    }
    if (status.value().output_status.error() != wh::core::errc::canceled) {
      return wh::core::result<wh::compose::graph_external_interrupt_resolution_kind>::failure(
          status.value().output_status.error());
    }
    if (!status.value().report.interrupt_resolution.has_value()) {
      return wh::core::result<wh::compose::graph_external_interrupt_resolution_kind>::failure(
          wh::core::errc::not_found);
    }
    return *status.value().report.interrupt_resolution;
  };

  wh::compose::graph_call_options immediate{};
  immediate.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
      .timeout = std::chrono::milliseconds{0},
      .mode = wh::compose::graph_interrupt_timeout_mode::immediate_rerun,
  };
  auto immediate_result = run_with_options(std::move(immediate));
  REQUIRE(immediate_result.has_value());
  REQUIRE(immediate_result.value() ==
          wh::compose::graph_external_interrupt_resolution_kind::immediate_rerun);

  wh::compose::graph_call_options wait_mode{};
  wait_mode.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
      .timeout = std::nullopt,
      .mode = wh::compose::graph_interrupt_timeout_mode::wait_inflight,
  };
  auto wait_result = run_with_options(std::move(wait_mode));
  REQUIRE(wait_result.has_value());
  REQUIRE(wait_result.value() ==
          wh::compose::graph_external_interrupt_resolution_kind::wait_inflight);
}

TEST_CASE("compose graph external interrupt persists entry and node pending inputs",
          "[core][compose][interrupt][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "external-rerun";

  wh::core::run_context context{};
  context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "external-rerun-int",
      .location = wh::core::make_address({"graph", "worker"}),
      .state = wh::core::any{std::monostate{}},
  };
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.save = write_options;

  wh::compose::graph_call_options call_options{};
  call_options.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
      .timeout = std::chrono::milliseconds{0},
      .mode = wh::compose::graph_interrupt_timeout_mode::immediate_rerun,
      .auto_persist_external_interrupt = true,
      .manual_persist_internal_interrupt = true,
  };
  auto status = invoke_graph_sync(graph, wh::core::any(9), context, call_options,
                                  std::addressof(services), controls);
  REQUIRE(status.has_value());
  REQUIRE(status.value().output_status.has_error());
  REQUIRE(status.value().output_status.error() == wh::core::errc::canceled);

  auto persisted = store.load(
      wh::compose::checkpoint_load_options{.checkpoint_id = std::string{"external-rerun"}});
  REQUIRE(persisted.has_value());
  auto start_input = checkpoint_entry_input(persisted.value());
  REQUIRE(start_input.has_value());
  REQUIRE(start_input.value() == 9);

  const auto *worker_payload = find_checkpoint_node_input(persisted.value(), "worker");
  REQUIRE(worker_payload != nullptr);
  auto worker_input = read_any<int>(*worker_payload);
  REQUIRE(worker_input.has_value());
  REQUIRE(worker_input.value() == 9);
}

TEST_CASE("compose graph external interrupt wait mode timeout persists pending node inputs",
          "[core][compose][interrupt][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("slow",
                          [](wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            std::this_thread::sleep_for(std::chrono::milliseconds{4});
                            return std::move(input);
                          })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "tail",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph.add_entry_edge("slow").has_value());
  REQUIRE(graph.add_edge("slow", "tail").has_value());
  REQUIRE(graph.add_exit_edge("tail").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "external-timeout-rerun";

  wh::core::run_context context{};
  context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "external-timeout-int",
      .location = wh::core::make_address({"graph", "slow"}),
      .state = wh::core::any{std::monostate{}},
  };
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.save = write_options;

  wh::compose::graph_call_options call_options{};
  call_options.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
      .timeout = std::chrono::milliseconds{1},
      .mode = wh::compose::graph_interrupt_timeout_mode::wait_inflight,
      .auto_persist_external_interrupt = true,
      .manual_persist_internal_interrupt = true,
  };
  auto status = invoke_graph_sync(graph, wh::core::any(12), context, call_options,
                                  std::addressof(services), controls);
  REQUIRE(status.has_value());
  REQUIRE(status.value().output_status.has_error());
  REQUIRE(status.value().output_status.error() == wh::core::errc::canceled);
  REQUIRE(status.value().report.interrupt_resolution.has_value());
  REQUIRE(*status.value().report.interrupt_resolution ==
          wh::compose::graph_external_interrupt_resolution_kind::wait_inflight);

  auto persisted = store.load(
      wh::compose::checkpoint_load_options{.checkpoint_id = std::string{"external-timeout-rerun"}});
  REQUIRE(persisted.has_value());
  auto start_input = checkpoint_entry_input(persisted.value());
  REQUIRE(start_input.has_value());
  REQUIRE(start_input.value() == 12);

  const auto *tail_payload = find_checkpoint_node_input(persisted.value(), "tail");
  REQUIRE(tail_payload != nullptr);
  auto tail_input = read_any<int>(*tail_payload);
  REQUIRE(tail_input.has_value());
  REQUIRE(tail_input.value() == 12);
}

TEST_CASE("compose tools stream external interrupt persists rerun batch across modes",
          "[core][compose][tools][interrupt][checkpoint]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};

  for (const auto mode : modes) {
    DYNAMIC_SECTION((mode == wh::compose::graph_runtime_mode::dag ? "dag" : "pregel")) {
      wh::compose::tool_registry tools{};
      tools.insert_or_assign(
          "echo",
          wh::compose::tool_entry{.async_stream =
                                      [](wh::compose::tool_call call,
                                         wh::tool::call_scope) -> wh::compose::tools_stream_sender {
            return stdexec::just(std::move(call.arguments)) |
                   stdexec::then(
                       [](std::string value) -> wh::core::result<wh::compose::graph_stream_reader> {
                         auto [writer, reader] = wh::compose::make_graph_stream();
                         auto wrote = writer.try_write(wh::core::any(std::move(value)));
                         if (wrote.has_error()) {
                           return wh::core::result<wh::compose::graph_stream_reader>::failure(
                               wrote.error());
                         }
                         auto closed = writer.close();
                         if (closed.has_error()) {
                           return wh::core::result<wh::compose::graph_stream_reader>::failure(
                               closed.error());
                         }
                         return std::move(reader);
                       });
          }});

      wh::compose::graph_compile_options options{};
      options.mode = mode;
      wh::compose::graph graph{std::move(options)};
      REQUIRE(graph
                  .add_tools(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                          wh::compose::node_contract::stream,
                                                          wh::compose::node_exec_mode::async>(
                      "tools", std::move(tools)))
                  .has_value());
      REQUIRE(graph.add_entry_edge("tools").has_value());
      REQUIRE(graph.add_exit_edge("tools", make_auto_contract_edge_options()).has_value());
      REQUIRE(graph.compile().has_value());

      wh::compose::checkpoint_store store{};
      wh::compose::checkpoint_save_options write_options{};
      write_options.checkpoint_id = mode == wh::compose::graph_runtime_mode::dag
                                        ? "tools-stream-int-dag"
                                        : "tools-stream-int-pregel";

      auto input_batch = make_tool_batch({wh::compose::tool_call{
          .call_id = "call-1",
          .tool_name = "echo",
          .arguments = "payload",
      }});

      wh::compose::graph_runtime_services services{};
      services.checkpoint.store = std::addressof(store);
      wh::compose::graph_invoke_controls controls{};
      controls.checkpoint.save = write_options;
      wh::core::run_context context{};
      context.interrupt_info = wh::core::interrupt_context{
          .interrupt_id = "tools-stream-int",
          .location = wh::core::make_address({"graph", "tools"}),
          .state = wh::core::any{std::monostate{}},
      };
      wh::compose::graph_call_options call_options{};
      call_options.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
          .timeout = std::chrono::milliseconds{0},
          .mode = wh::compose::graph_interrupt_timeout_mode::immediate_rerun,
          .auto_persist_external_interrupt = true,
          .manual_persist_internal_interrupt = true,
      };
      auto status = invoke_graph_sync(graph, wh::core::any(input_batch), context, call_options,
                                      std::addressof(services), controls);
      REQUIRE(status.has_value());
      REQUIRE(status.value().output_status.has_error());
      REQUIRE(status.value().output_status.error() == wh::core::errc::canceled);

      auto persisted = store.load(
          wh::compose::checkpoint_load_options{.checkpoint_id = write_options.checkpoint_id});
      REQUIRE(persisted.has_value());

      const auto *entry_payload = find_checkpoint_entry_input(persisted.value());
      REQUIRE(entry_payload != nullptr);
      auto start_batch = read_any<wh::compose::tool_batch>(*entry_payload);
      REQUIRE(start_batch.has_value());
      REQUIRE(start_batch.value().calls.size() == 1U);
      REQUIRE(start_batch.value().calls.front().tool_name == "echo");
      REQUIRE(start_batch.value().calls.front().arguments == "payload");

      const auto *tools_payload = find_checkpoint_node_input(persisted.value(), "tools");
      REQUIRE(tools_payload != nullptr);
      auto tools_batch = read_any<wh::compose::tool_batch>(*tools_payload);
      REQUIRE(tools_batch.has_value());
      REQUIRE(tools_batch.value().calls.size() == 1U);
      REQUIRE(tools_batch.value().calls.front().call_id == "call-1");
      REQUIRE(tools_batch.value().calls.front().tool_name == "echo");
      REQUIRE(tools_batch.value().calls.front().arguments == "payload");

      wh::compose::graph_invoke_controls resume_controls{};
      resume_controls.checkpoint.load =
          wh::compose::checkpoint_load_options{.checkpoint_id = write_options.checkpoint_id};
      resume_controls.checkpoint.save = write_options;
      wh::core::run_context resume_context{};
      auto resumed = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(),
                                       resume_context, resume_controls, std::addressof(services));
      REQUIRE(resumed.has_value());
      REQUIRE(resumed.value().output_status.has_error());
      REQUIRE(resumed.value().output_status.error() == wh::core::errc::canceled);
      REQUIRE(resume_context.interrupt_info.has_value());
      REQUIRE(resume_context.interrupt_info->interrupt_id == "tools-stream-int");
    }
  }
}

TEST_CASE("compose subgraph stream external interrupt persists pending input across modes",
          "[core][compose][subgraph][interrupt][checkpoint]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};

  for (const auto mode : modes) {
    DYNAMIC_SECTION((mode == wh::compose::graph_runtime_mode::dag ? "dag" : "pregel")) {
      wh::compose::graph child{wh::compose::graph_boundary{
          .output = wh::compose::node_contract::stream,
      }};
      REQUIRE(
          child
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "leaf",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto typed = read_any<int>(input);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          typed.error());
                    }
                    return make_int_graph_stream({typed.value(), typed.value() + 1});
                  })
              .has_value());
      REQUIRE(child.add_entry_edge("leaf").has_value());
      REQUIRE(child.add_exit_edge("leaf").has_value());
      REQUIRE(child.compile().has_value());

      wh::compose::graph_compile_options options{};
      options.mode = mode;
      wh::compose::graph graph{std::move(options)};
      REQUIRE(graph.add_subgraph(wh::compose::make_subgraph_node("child", std::move(child)))
                  .has_value());
      REQUIRE(graph.add_entry_edge("child").has_value());
      REQUIRE(graph.add_exit_edge("child", make_auto_contract_edge_options()).has_value());
      REQUIRE(graph.compile().has_value());

      wh::compose::checkpoint_store store{};
      wh::compose::checkpoint_save_options write_options{};
      write_options.checkpoint_id = mode == wh::compose::graph_runtime_mode::dag
                                        ? "subgraph-stream-int-dag"
                                        : "subgraph-stream-int-pregel";

      wh::compose::graph_runtime_services services{};
      services.checkpoint.store = std::addressof(store);
      wh::compose::graph_invoke_controls controls{};
      controls.checkpoint.save = write_options;
      wh::core::run_context context{};
      context.interrupt_info = wh::core::interrupt_context{
          .interrupt_id = "subgraph-stream-int",
          .location = wh::core::make_address({"graph", "child"}),
          .state = wh::core::any{std::monostate{}},
      };
      wh::compose::graph_call_options call_options{};
      call_options.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
          .timeout = std::chrono::milliseconds{0},
          .mode = wh::compose::graph_interrupt_timeout_mode::immediate_rerun,
          .auto_persist_external_interrupt = true,
          .manual_persist_internal_interrupt = true,
      };
      auto status = invoke_graph_sync(graph, wh::core::any(9), context, call_options,
                                      std::addressof(services), controls);
      REQUIRE(status.has_value());
      REQUIRE(status.value().output_status.has_error());
      REQUIRE(status.value().output_status.error() == wh::core::errc::canceled);

      auto persisted = store.load(
          wh::compose::checkpoint_load_options{.checkpoint_id = write_options.checkpoint_id});
      REQUIRE(persisted.has_value());

      auto start_input = checkpoint_entry_input(persisted.value());
      REQUIRE(start_input.has_value());
      REQUIRE(start_input.value() == 9);

      const auto *child_payload = find_checkpoint_node_input(persisted.value(), "child");
      REQUIRE(child_payload != nullptr);
      auto child_input = read_any<int>(*child_payload);
      REQUIRE(child_input.has_value());
      REQUIRE(child_input.value() == 9);

      wh::compose::graph_invoke_controls resume_controls{};
      resume_controls.checkpoint.load =
          wh::compose::checkpoint_load_options{.checkpoint_id = write_options.checkpoint_id};
      resume_controls.checkpoint.save = write_options;
      wh::core::run_context resume_context{};
      auto resumed = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(),
                                       resume_context, resume_controls, std::addressof(services));
      REQUIRE(resumed.has_value());
      REQUIRE(resumed.value().output_status.has_error());
      REQUIRE(resumed.value().output_status.error() == wh::core::errc::canceled);
      REQUIRE(resume_context.interrupt_info.has_value());
      REQUIRE(resume_context.interrupt_info->interrupt_id == "subgraph-stream-int");
    }
  }
}
