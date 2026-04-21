#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/adk/interrupt.hpp"

TEST_CASE("adk interrupt projects only agent and tool segments from runtime paths",
          "[UT][wh/adk/interrupt.hpp][project_interrupt_run_path][condition][branch][boundary]") {
  REQUIRE(wh::adk::project_interrupt_run_path(wh::core::address{"graph", "root", "agent", "planner",
                                                                "node", "inner", "tool", "search",
                                                                "call-7"})
              .to_string("/") == "agent/planner/tool/search/call-7");
  REQUIRE(
      wh::adk::project_interrupt_run_path(wh::core::address{"graph", "root", "misc", "x"}).empty());
}

TEST_CASE(
    "adk interrupt current_interrupt_info covers missing exact descendant and none target states",
    "[UT][wh/adk/interrupt.hpp][current_interrupt_info][condition][branch][boundary]") {
  wh::core::run_context missing_context{};
  auto missing = wh::adk::current_interrupt_info(missing_context);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  wh::core::run_context exact_context{};
  exact_context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "interrupt-exact",
      .location = wh::core::address{"graph", "root", "agent", "worker", "tool", "search", "call-1"},
      .state = wh::core::any(std::string{"state"}),
      .layer_payload = wh::core::any(7),
      .trigger_reason = "exact",
  };
  REQUIRE(wh::compose::add_resume_target(
              exact_context.resume_info.emplace(), "interrupt-exact",
              wh::core::address{"graph", "root", "agent", "worker", "tool", "search", "call-1"},
              std::string{"resume"})
              .has_value());
  auto exact = wh::adk::current_interrupt_info(exact_context);
  REQUIRE(exact.has_value());
  REQUIRE(exact->target_kind == wh::adk::interrupt_target_kind::exact);
  REQUIRE(exact->run_path.to_string("/") == "agent/worker/tool/search/call-1");
  REQUIRE(*wh::core::any_cast<std::string>(&exact->state) == "state");
  REQUIRE(*wh::core::any_cast<int>(&exact->payload) == 7);

  wh::core::run_context descendant_context{};
  descendant_context.interrupt_info = wh::core::interrupt_context{
      .interrupt_id = "interrupt-parent",
      .location = wh::core::address{"graph", "root", "agent", "worker"},
  };
  REQUIRE(wh::compose::add_resume_target(
              descendant_context.resume_info.emplace(), "interrupt-child",
              wh::core::address{"graph", "root", "agent", "worker", "tool", "search", "call-2"},
              std::string{"resume"})
              .has_value());
  auto descendant = wh::adk::current_interrupt_info(descendant_context);
  REQUIRE(descendant.has_value());
  REQUIRE(descendant->target_kind == wh::adk::interrupt_target_kind::descendant);

  wh::core::interrupt_context none_context{
      .interrupt_id = "interrupt-none",
      .location = wh::core::address{"graph", "root", "agent", "worker"},
      .used = true,
      .trigger_reason = "none",
  };
  auto none = wh::adk::make_interrupt_info(none_context);
  REQUIRE(none.has_value());
  REQUIRE(none->target_kind == wh::adk::interrupt_target_kind::none);
  REQUIRE(none->used);
  REQUIRE(none->trigger_reason == "none");
}

TEST_CASE("adk interrupt info fails instead of silently dropping move-only payloads",
          "[UT][wh/adk/interrupt.hpp][make_interrupt_info][boundary]") {
  wh::core::interrupt_context context{
      .interrupt_id = "move-only",
      .location = wh::core::address{"agent", "worker"},
      .state = wh::core::any{std::make_unique<int>(7)},
  };

  auto info = wh::adk::make_interrupt_info(context);
  REQUIRE(info.has_error());
}

TEST_CASE("adk interrupt apply_interrupt_patch lowers approve edit and reject decisions",
          "[UT][wh/adk/interrupt.hpp][apply_interrupt_patch][condition][branch][boundary]") {
  const wh::core::interrupt_context interrupt{
      .interrupt_id = "interrupt-1",
      .location = wh::core::address{"agent", "worker"},
      .state = wh::core::any(std::string{"original"}),
  };

  wh::core::resume_state approved{};
  REQUIRE(wh::adk::apply_interrupt_patch(approved, interrupt,
                                         wh::adk::interrupt_patch{
                                             .resolution = wh::adk::interrupt_resolution::approve,
                                             .audit =
                                                 wh::adk::interrupt_audit{
                                                     .audit_id = "audit-1",
                                                     .actor = "tester",
                                                     .reason = "ok",
                                                 },
                                         })
              .has_value());
  auto approved_patch = approved.peek<wh::compose::resume_patch>("interrupt-1");
  REQUIRE(approved_patch.has_value());
  REQUIRE(approved_patch->get().decision == wh::compose::interrupt_decision_kind::approve);
  REQUIRE(*wh::core::any_cast<std::string>(&approved_patch->get().data) == "original");

  wh::core::resume_state edited{};
  REQUIRE(wh::adk::apply_interrupt_patch(edited, interrupt,
                                         wh::adk::interrupt_patch{
                                             .resolution = wh::adk::interrupt_resolution::edit,
                                             .payload = wh::core::any(std::string{"edited"}),
                                         })
              .has_value());
  auto edited_patch = edited.peek<wh::compose::resume_patch>("interrupt-1");
  REQUIRE(edited_patch.has_value());
  REQUIRE(edited_patch->get().decision == wh::compose::interrupt_decision_kind::edit);
  REQUIRE(*wh::core::any_cast<std::string>(&edited_patch->get().data) == "edited");

  wh::core::resume_state rejected{};
  auto rejected_status =
      wh::adk::apply_interrupt_patch(rejected, interrupt,
                                     wh::adk::interrupt_patch{
                                         .resolution = wh::adk::interrupt_resolution::reject,
                                     });
  REQUIRE(rejected_status.has_error());
  REQUIRE(rejected_status.error() == wh::core::errc::canceled);
}

TEST_CASE(
    "adk interrupt apply_interrupt_patch_batch updates matched contexts and rejects missing ids",
    "[UT][wh/adk/interrupt.hpp][apply_interrupt_patch_batch][condition][branch][boundary]") {
  const wh::core::interrupt_context interrupt{
      .interrupt_id = "interrupt-1",
      .location = wh::core::address{"agent", "worker"},
      .state = wh::core::any(std::string{"original"}),
  };
  const std::vector<wh::core::interrupt_context> contexts{interrupt};
  const std::vector<wh::adk::interrupt_patch_item> patches{
      {.interrupt_id = "interrupt-1",
       .patch = {.resolution = wh::adk::interrupt_resolution::approve}},
      {.interrupt_id = "interrupt-1",
       .patch = {.resolution = wh::adk::interrupt_resolution::edit,
                 .payload = wh::core::any(std::string{"override"})}},
  };

  wh::core::resume_state batch_state{};
  REQUIRE(wh::adk::apply_interrupt_patch_batch(batch_state, contexts, patches).has_value());
  auto batch_patch = batch_state.peek<wh::compose::resume_patch>("interrupt-1");
  REQUIRE(batch_patch.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&batch_patch->get().data) == "override");

  const std::vector<wh::adk::interrupt_patch_item> missing_patch{
      {.interrupt_id = "missing", .patch = {.resolution = wh::adk::interrupt_resolution::approve}},
  };
  auto missing_batch = wh::adk::apply_interrupt_patch_batch(batch_state, contexts, missing_patch);
  REQUIRE(missing_batch.has_error());
  REQUIRE(missing_batch.error() == wh::core::errc::not_found);
}
