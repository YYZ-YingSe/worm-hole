#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "wh/adk/interrupt.hpp"
#include "wh/compose/runtime/resume.hpp"

TEST_CASE("adk interrupt run path projection keeps only agent and tool segments",
          "[core][adk][condition]") {
  const auto projected = wh::adk::project_interrupt_run_path(
      wh::core::address{"graph", "root", "agent", "planner", "node", "inner",
                        "tool", "search", "call-7", "compose", "lane"});
  REQUIRE(projected.to_string("/") == "agent/planner/tool/search/call-7");
}

TEST_CASE("adk interrupt info distinguishes exact and descendant resume targets",
          "[core][adk][condition]") {
  wh::core::run_context exact_context{};
  exact_context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "interrupt-exact",
      .location =
          wh::core::address{"graph", "root", "agent", "worker", "tool", "search",
                            "call-1"},
      .state = wh::core::any(std::string{"state"}),
      .layer_payload = wh::core::any(7),
      .trigger_reason = "exact",
  };
  REQUIRE(wh::compose::add_resume_target(
              exact_context.resume_info.emplace(), "interrupt-exact",
              wh::core::address{"graph", "root", "agent", "worker", "tool",
                                "search", "call-1"},
              std::string{"resume"})
              .has_value());

  auto exact = wh::adk::current_interrupt_info(exact_context);
  REQUIRE(exact.has_value());
  REQUIRE(exact.value().run_path.to_string("/") ==
          "agent/worker/tool/search/call-1");
  REQUIRE(exact.value().target_kind == wh::adk::interrupt_target_kind::exact);
  REQUIRE(wh::core::any_cast<std::string>(&exact.value().state) != nullptr);
  REQUIRE(*wh::core::any_cast<std::string>(&exact.value().state) == "state");

  wh::core::run_context descendant_context{};
  descendant_context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "interrupt-parent",
      .location = wh::core::address{"graph", "root", "agent", "worker"},
      .trigger_reason = "descendant",
  };
  REQUIRE(wh::compose::add_resume_target(
              descendant_context.resume_info.emplace(), "interrupt-child",
              wh::core::address{"graph", "root", "agent", "worker", "tool",
                                "search", "call-2"},
              std::string{"resume"})
              .has_value());

  auto descendant = wh::adk::current_interrupt_info(descendant_context);
  REQUIRE(descendant.has_value());
  REQUIRE(descendant.value().run_path.to_string("/") == "agent/worker");
  REQUIRE(descendant.value().target_kind ==
          wh::adk::interrupt_target_kind::descendant);
}

TEST_CASE("adk interrupt patch lowers approve edit and reject onto compose resume state",
          "[core][adk][condition]") {
  const wh::core::interrupt_context context{
      .interrupt_id = "interrupt-1",
      .location = wh::core::address{"agent", "worker"},
      .state = wh::core::any(std::string{"original"}),
  };

  wh::core::resume_state approved{};
  REQUIRE(wh::adk::apply_interrupt_patch(
              approved, context,
              wh::adk::interrupt_patch{
                  .resolution = wh::adk::interrupt_resolution::approve,
                  .audit = wh::adk::interrupt_audit{
                      .audit_id = "audit-1",
                      .actor = "tester",
                      .reason = "ok",
                  },
              })
              .has_value());
  auto approved_patch = approved.peek<wh::compose::resume_patch>("interrupt-1");
  REQUIRE(approved_patch.has_value());
  REQUIRE(approved_patch.value().get().decision ==
          wh::compose::interrupt_decision_kind::approve);
  REQUIRE(*wh::core::any_cast<std::string>(&approved_patch.value().get().data) ==
          "original");
  REQUIRE(approved_patch.value().get().audit.audit_id == "audit-1");

  wh::core::resume_state edited{};
  REQUIRE(wh::adk::apply_interrupt_patch(
              edited, context,
              wh::adk::interrupt_patch{
                  .resolution = wh::adk::interrupt_resolution::edit,
                  .payload = wh::core::any(std::string{"edited"}),
              })
              .has_value());
  auto edited_patch = edited.peek<wh::compose::resume_patch>("interrupt-1");
  REQUIRE(edited_patch.has_value());
  REQUIRE(edited_patch.value().get().decision ==
          wh::compose::interrupt_decision_kind::edit);
  REQUIRE(*wh::core::any_cast<std::string>(&edited_patch.value().get().data) ==
          "edited");

  wh::core::resume_state rejected{};
  auto rejected_status = wh::adk::apply_interrupt_patch(
      rejected, context,
      wh::adk::interrupt_patch{
          .resolution = wh::adk::interrupt_resolution::reject,
      });
  REQUIRE(rejected_status.has_error());
  REQUIRE(rejected_status.error() == wh::core::errc::canceled);
}

TEST_CASE("adk interrupt patch batch keeps last write for same interrupt id",
          "[core][adk][condition]") {
  const std::vector<wh::core::interrupt_context> contexts{
      wh::core::interrupt_context{
          .interrupt_id = "interrupt-1",
          .location = wh::core::address{"agent", "worker"},
          .state = wh::core::any(std::string{"original"}),
      },
  };

  wh::core::resume_state state{};
  const std::vector<wh::adk::interrupt_patch_item> patches{
      wh::adk::interrupt_patch_item{
          .interrupt_id = "interrupt-1",
          .patch =
              wh::adk::interrupt_patch{
                  .resolution = wh::adk::interrupt_resolution::approve,
              },
      },
      wh::adk::interrupt_patch_item{
          .interrupt_id = "interrupt-1",
          .patch =
              wh::adk::interrupt_patch{
                  .resolution = wh::adk::interrupt_resolution::edit,
                  .payload = wh::core::any(std::string{"override"}),
              },
      },
  };

  REQUIRE(wh::adk::apply_interrupt_patch_batch(state, contexts, patches).has_value());
  auto patch = state.peek<wh::compose::resume_patch>("interrupt-1");
  REQUIRE(patch.has_value());
  REQUIRE(patch.value().get().decision ==
          wh::compose::interrupt_decision_kind::edit);
  REQUIRE(*wh::core::any_cast<std::string>(&patch.value().get().data) ==
          "override");
}
