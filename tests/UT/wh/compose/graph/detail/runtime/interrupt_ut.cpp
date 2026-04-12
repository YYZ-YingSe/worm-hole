#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "wh/compose/graph/detail/runtime/interrupt.hpp"

TEST_CASE("runtime interrupt helpers evaluate hooks and resolve external policy modes",
          "[UT][wh/compose/graph/detail/runtime/interrupt.hpp][evaluate_hook][condition][branch][boundary]") {
  using namespace wh::compose::detail::interrupt_runtime;

  wh::core::run_context context{};
  auto empty = evaluate_hook(
      context, wh::compose::graph_interrupt_node_hook{nullptr}, "worker",
      wh::compose::graph_value{1});
  REQUIRE(empty.has_value());
  REQUIRE_FALSE(empty->has_value());

  auto signaled = evaluate_hook(
      context,
      wh::compose::graph_interrupt_node_hook{
          [](const std::string_view node_key, const wh::compose::graph_value &,
             wh::core::run_context &)
              -> wh::core::result<
                  std::optional<wh::core::interrupt_signal>> {
            return std::optional<wh::core::interrupt_signal>{
                wh::compose::make_interrupt_signal(
                    "interrupt-" + std::string{node_key},
                    wh::core::make_address({"graph", std::string{node_key}}),
                    7)};
          }},
      "worker", wh::compose::graph_value{2});
  REQUIRE(signaled.has_value());
  REQUIRE(signaled->has_value());
  REQUIRE(signaled->value().interrupt_id == "interrupt-worker");

  wh::compose::graph_external_interrupt_policy policy{};
  policy.mode = wh::compose::graph_interrupt_timeout_mode::immediate_rerun;
  REQUIRE(resolve_external_resolution_kind(policy) ==
          wh::compose::graph_external_interrupt_resolution_kind::immediate_rerun);
  policy.mode = wh::compose::graph_interrupt_timeout_mode::wait_inflight;
  policy.timeout = std::chrono::milliseconds{0};
  REQUIRE(resolve_external_resolution_kind(policy) ==
          wh::compose::graph_external_interrupt_resolution_kind::immediate_rerun);
  policy.timeout = std::chrono::milliseconds{10};
  REQUIRE(resolve_external_resolution_kind(policy) ==
          wh::compose::graph_external_interrupt_resolution_kind::wait_inflight);
}

TEST_CASE("runtime interrupt helpers apply resume controls and external boundary persistence",
          "[UT][wh/compose/graph/detail/runtime/interrupt.hpp][handle_external_boundary][condition][branch][boundary]") {
  using namespace wh::compose::detail::interrupt_runtime;

  wh::core::run_context context{};
  context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "ctx",
      .location = wh::core::make_address({"root", "node"}),
      .state = wh::core::any{5},
  };

  wh::compose::detail::runtime_state::invoke_config config{};
  config.resume_decision = wh::compose::interrupt_resume_decision{
      .interrupt_context_id = "ctx",
      .decision = wh::compose::interrupt_decision_kind::approve,
  };
  config.batch_resume_items.push_back(
      wh::compose::resume_batch_item{
          .interrupt_context_id = "ctx",
          .data = wh::core::any{9},
      });
  REQUIRE(apply_runtime_resume_controls(context, config).has_value());
  REQUIRE(context.resume_info.has_value());
  REQUIRE(context.resume_info->interrupt_ids(true).size() == 1U);

  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  wh::compose::graph_external_interrupt_policy_latch latch{};
  external_interrupt_boundary_state boundary_state{};
  wh::compose::graph_external_interrupt_policy immediate_policy{
      .timeout = std::chrono::milliseconds{0},
      .mode = wh::compose::graph_interrupt_timeout_mode::wait_inflight,
  };

  bool persisted = false;
  auto immediate = handle_external_boundary(
      context, outputs, latch, immediate_policy, boundary_state,
      [&](const bool external_interrupt) -> wh::core::result<void> {
        persisted = external_interrupt;
        return {};
      });
  REQUIRE(immediate.has_value());
  REQUIRE(immediate.value());
  REQUIRE(persisted);
  REQUIRE(outputs.external_interrupt_resolution.has_value());
  REQUIRE(outputs.external_interrupt_resolution.value() ==
          wh::compose::graph_external_interrupt_resolution_kind::immediate_rerun);

  outputs = {};
  latch = {};
  boundary_state = {};
  persisted = false;
  wh::compose::graph_external_interrupt_policy wait_policy{
      .timeout = std::chrono::milliseconds{50},
      .mode = wh::compose::graph_interrupt_timeout_mode::wait_inflight,
  };
  auto waiting = handle_external_boundary(
      outputs, latch, wait_policy, boundary_state,
      [&](const bool) -> wh::core::result<void> {
        persisted = true;
        return {};
      });
  REQUIRE(waiting.has_value());
  REQUIRE_FALSE(waiting.value());
  REQUIRE(boundary_state.wait_mode_active);
  REQUIRE(boundary_state.deadline.has_value());
  REQUIRE_FALSE(persisted);
}
