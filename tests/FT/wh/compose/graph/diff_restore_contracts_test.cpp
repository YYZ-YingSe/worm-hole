#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "wh/compose/graph.hpp"
#include "wh/compose/graph/diff.hpp"
#include "wh/compose/graph/restore_validation.hpp"
#include "wh/compose/runtime.hpp"
#include "wh/core/any.hpp"
#include "helper/static_thread_scheduler.hpp"
#include "helper/sender_capture.hpp"

namespace {

using graph_result = wh::core::result<wh::compose::graph_invoke_result>;

template <typename value_t>
[[nodiscard]] auto read_any(const wh::core::any &value)
    -> wh::core::result<value_t> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value);
      typed != nullptr) {
    if constexpr (std::copy_constructible<value_t>) {
      return *typed;
    } else {
      return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
    }
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] auto read_any(wh::core::any &&value)
    -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto int_payload(const wh::compose::graph_value &value)
    -> wh::core::result<int> {
  return read_any<int>(value);
}

[[nodiscard]] auto build_add_graph(std::string key, const int delta)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph graph{};
  const std::string node_key = key;
  auto added = graph.add_lambda(
      std::move(key),
      [delta](const wh::compose::graph_value &input, wh::core::run_context &,
              const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto typed = int_payload(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              typed.error());
        }
          return wh::core::any(typed.value() + delta);
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  auto first_edge = graph.add_entry_edge(node_key);
  if (first_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(first_edge.error());
  }
  auto last_edge = graph.add_exit_edge(node_key);
  if (last_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(last_edge.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return std::move(graph);
}

inline auto set_checkpoint_start_input(wh::compose::checkpoint_state &checkpoint,
                                       const int value) -> void {
  checkpoint.rerun_inputs.insert_or_assign(
      std::string{wh::compose::graph_start_node_key},
      wh::core::any(value));
}

[[nodiscard]] auto invoke_graph_int(wh::compose::graph &graph,
                                    wh::core::run_context &context,
                                    wh::compose::graph_value input,
                                    const wh::compose::graph_runtime_services *services =
                                        nullptr,
                                    wh::compose::graph_invoke_controls controls = {})
    -> wh::core::result<int> {
  wh::testing::helper::static_thread_scheduler_helper scheduler{1U};
  graph_result result{};
  wh::compose::graph_invoke_request request{};
  request.input = std::move(input);
  request.services = services;
  request.controls = std::move(controls);
  const bool completed = wh::testing::helper::wait_for_value(
      graph.invoke(context, std::move(request)), result, std::chrono::seconds{1},
      scheduler.env());
  if (!completed) {
    return wh::core::result<int>::failure(wh::core::errc::timeout);
  }
  if (result.has_error()) {
    return wh::core::result<int>::failure(result.error());
  }
  if (result.value().output_status.has_error()) {
    return wh::core::result<int>::failure(result.value().output_status.error());
  }
  return int_payload(result.value().output_status.value());
}

[[nodiscard]] auto contains_issue(
    const wh::compose::restore_validation_result &validation,
    const wh::compose::restore_issue_kind kind) -> bool {
  return std::any_of(validation.issues.begin(), validation.issues.end(),
                     [kind](const wh::compose::restore_issue &issue) {
                       return issue.kind == kind;
                     });
}

} // namespace

TEST_CASE("compose diff_graph reports compile-visible topology changes",
          "[core][compose][diff][condition]") {
  auto baseline = build_add_graph("gate", 1);
  auto candidate = build_add_graph("other", 2);
  REQUIRE(baseline.has_value());
  REQUIRE(candidate.has_value());

  auto diff =
      wh::compose::diff_graph(baseline.value(), candidate.value());
  REQUIRE(diff.has_value());
  REQUIRE(diff.value().contains(wh::compose::graph_diff_kind::node_removed));
  REQUIRE(diff.value().contains(wh::compose::graph_diff_kind::node_added));
  REQUIRE(diff.value().contains(wh::compose::graph_diff_kind::edge_removed));
  REQUIRE(diff.value().contains(wh::compose::graph_diff_kind::edge_added));
}

TEST_CASE("compose validate_restore accepts matching snapshots and rejects changed graphs",
          "[core][compose][restore][condition]") {
  auto baseline = build_add_graph("gate", 1);
  auto candidate = build_add_graph("other", 2);
  REQUIRE(baseline.has_value());
  REQUIRE(candidate.has_value());

  wh::compose::checkpoint_state checkpoint{};
  checkpoint.checkpoint_id = "job-42";
  checkpoint.restore_shape = baseline.value().restore_shape();
  checkpoint.node_states.push_back(wh::compose::graph_node_state{
      .key = "gate",
  });
  set_checkpoint_start_input(checkpoint, 5);

  auto matching = wh::compose::validate_restore(baseline.value(), checkpoint);
  REQUIRE(matching.has_value());
  REQUIRE(matching.value().restorable);
  REQUIRE(matching.value().diff.entries.empty());

  auto changed = wh::compose::validate_restore(candidate.value(), checkpoint);
  REQUIRE(changed.has_value());
  REQUIRE_FALSE(changed.value().restorable);
  REQUIRE(changed.value().diff.contains(
      wh::compose::restore_diff_kind::node_removed));
  REQUIRE(contains_issue(changed.value(),
                         wh::compose::restore_issue_kind::graph_changed));
  REQUIRE(contains_issue(changed.value(),
                         wh::compose::restore_issue_kind::missing_node_state));
}

TEST_CASE("compose validate_restore ignores graph-name drift",
          "[core][compose][restore][condition]") {
  wh::compose::graph checkpoint_graph{
      wh::compose::graph_compile_options{.name = "graph-renamed"}};
  REQUIRE(checkpoint_graph
              .add_lambda("gate",
                          [](const wh::compose::graph_value &input,
                             wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            return input;
                          })
              .has_value());
  REQUIRE(checkpoint_graph.add_entry_edge("gate").has_value());
  REQUIRE(checkpoint_graph.add_exit_edge("gate").has_value());
  REQUIRE(checkpoint_graph.compile().has_value());

  auto graph = build_add_graph("gate", 1);
  REQUIRE(graph.has_value());

  wh::compose::checkpoint_state checkpoint{};
  checkpoint.checkpoint_id = "job-rename";
  checkpoint.restore_shape = checkpoint_graph.restore_shape();
  set_checkpoint_start_input(checkpoint, 5);
  REQUIRE(wh::compose::add_resume_target(checkpoint.resume_snapshot,
                                         "interrupt-1",
                                         wh::core::address{"graph-renamed",
                                                           "gate"},
                                         std::string{"payload"})
              .has_value());

  auto validation = wh::compose::validate_restore(graph.value(), checkpoint);
  REQUIRE(validation.has_value());
  REQUIRE(validation.value().restorable);
  REQUIRE(validation.value().diff.entries.empty());
  REQUIRE_FALSE(contains_issue(validation.value(),
                               wh::compose::restore_issue_kind::graph_changed));
  REQUIRE(validation.value().issues.empty());
}

TEST_CASE("compose checkpoint services and controls drive runtime restore through public API",
          "[core][compose][checkpoint][condition]") {
  auto graph = build_add_graph("gate", 1);
  REQUIRE(graph.has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_state checkpoint{};
  checkpoint.checkpoint_id = "job-42";
  checkpoint.restore_shape = graph.value().restore_shape();
  set_checkpoint_start_input(checkpoint, 5);
  REQUIRE(store.save(checkpoint, wh::compose::checkpoint_save_options{
                                     .checkpoint_id = std::string{"job-42"},
                                 })
              .has_value());

  auto restore_plan = store.prepare_restore(
      wh::compose::checkpoint_load_options{
          .checkpoint_id = std::string{"job-42"},
      });
  REQUIRE(restore_plan.has_value());
  REQUIRE(restore_plan.value().restore_from_checkpoint);
  REQUIRE(restore_plan.value().checkpoint.has_value());

  auto validation =
      wh::compose::validate_restore(graph.value(),
                                    *restore_plan.value().checkpoint);
  REQUIRE(validation.has_value());
  REQUIRE(validation.value().restorable);

  wh::core::run_context context{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.load = wh::compose::checkpoint_load_options{
      .checkpoint_id = std::string{"job-42"},
  };
  controls.checkpoint.save = wh::compose::checkpoint_save_options{
      .checkpoint_id = std::string{"job-42"},
  };

  auto invoked =
      invoke_graph_int(graph.value(), context,
                       wh::core::any(std::monostate{}), &services,
                       std::move(controls));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value() == 6);
}
