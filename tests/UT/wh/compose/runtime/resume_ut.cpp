#include <catch2/catch_test_macros.hpp>

#include "wh/compose/runtime/resume.hpp"

TEST_CASE("compose runtime resume helpers add targets and classify matches",
          "[UT][wh/compose/runtime/resume.hpp][add_resume_target][condition][branch][boundary]") {
  wh::core::resume_state state{};
  REQUIRE(wh::compose::add_resume_target(
              state, "r1", wh::core::make_address({"root", "child"}), 7)
              .has_value());

  auto exact = wh::compose::classify_resume_target_match(
      state, wh::core::make_address({"root", "child"}));
  REQUIRE(exact.in_resume_flow);
  REQUIRE(exact.match_kind == wh::compose::resume_target_match_kind::exact);
  REQUIRE_FALSE(exact.should_reinterrupt);

  auto descendant = wh::compose::classify_resume_target_match(
      state, wh::core::make_address({"root"}));
  REQUIRE(descendant.match_kind ==
          wh::compose::resume_target_match_kind::descendant);

  auto none = wh::compose::classify_resume_target_match(
      state, wh::core::make_address({"other"}));
  REQUIRE(none.should_reinterrupt);
}

TEST_CASE("compose runtime resume helpers apply decisions batches and reinterrupt collection",
          "[UT][wh/compose/runtime/resume.hpp][apply_resume_decision][condition][branch][boundary]") {
  wh::core::interrupt_context context{
      .interrupt_id = "ctx",
      .location = wh::core::make_address({"root", "node"}),
      .state = wh::core::any{5},
  };

  wh::core::resume_state state{};
  auto approved = wh::compose::apply_resume_decision(
      state, context,
      wh::compose::interrupt_resume_decision{
          .interrupt_context_id = "ctx",
          .decision = wh::compose::interrupt_decision_kind::approve,
      });
  REQUIRE(approved.has_value());

  wh::core::resume_state batch_state{};
  std::vector<wh::compose::resume_batch_item> items{
      {.interrupt_context_id = "ctx", .data = wh::core::any{9}},
  };
  auto batch = wh::compose::apply_resume_batch(batch_state, std::span{&context, 1},
                                               items);
  REQUIRE(batch.has_value());

  auto reinterrupts = wh::compose::collect_reinterrupts(
      batch_state, std::span{&context, 1});
  REQUIRE(reinterrupts.empty());

  wh::core::resume_state empty_state{};
  auto unmatched =
      wh::compose::collect_reinterrupts(empty_state, std::span{&context, 1});
  REQUIRE(unmatched.size() == 1U);
  REQUIRE(unmatched.front().interrupt_id == "ctx");
}

TEST_CASE("compose runtime resume helpers merge consume subtree operations and decision failures",
          "[UT][wh/compose/runtime/resume.hpp][merge_resume_state][condition][branch][boundary]") {
  wh::core::resume_state state{};
  REQUIRE(wh::compose::add_resume_target(
              state, "a", wh::core::make_address({"root", "a"}), 1)
              .has_value());
  REQUIRE(wh::compose::add_resume_target(
              state, "b", wh::core::make_address({"root", "a", "leaf"}), 2)
              .has_value());

  wh::core::resume_state delta{};
  REQUIRE(wh::compose::add_resume_target(
              delta, "c", wh::core::make_address({"root", "c"}), 3)
              .has_value());
  REQUIRE(wh::compose::merge_resume_state(state, delta).has_value());

  wh::core::resume_state moved_delta{};
  REQUIRE(wh::compose::add_resume_target(
              moved_delta, "d", wh::core::make_address({"root", "d"}), 4)
              .has_value());
  REQUIRE(wh::compose::merge_resume_state(state, std::move(moved_delta)).has_value());

  auto next = wh::compose::next_resume_points(
      state, wh::core::make_address({"root"}));
  REQUIRE(next == std::vector<std::string>{"a", "c", "d"});

  auto subtree = wh::compose::collect_resume_subtree_ids(
      state, wh::core::make_address({"root", "a"}));
  REQUIRE(subtree == std::vector<std::string>{"a", "b"});

  REQUIRE(wh::compose::mark_resume_subtree_used(
              state, wh::core::make_address({"root", "a"})) == 2U);
  REQUIRE(wh::compose::erase_resume_subtree(
              state, wh::core::make_address({"root", "a"}),
              wh::core::resume_subtree_erase_options{.include_used = false}) == 0U);
  REQUIRE(wh::compose::erase_resume_subtree(
              state, wh::core::make_address({"root", "a"}),
              wh::core::resume_subtree_erase_options{.include_used = true}) == 2U);

  auto consumed = wh::compose::consume_resume_data<int>(state, "c");
  REQUIRE(consumed.has_value());
  REQUIRE(consumed.value() == 3);

  wh::core::interrupt_context context{
      .interrupt_id = "ctx",
      .location = wh::core::make_address({"root", "node"}),
      .state = wh::core::any{5},
  };

  auto mismatched = wh::compose::apply_resume_decision(
      state, context,
      wh::compose::interrupt_resume_decision{
          .interrupt_context_id = "other",
          .decision = wh::compose::interrupt_decision_kind::approve,
      });
  REQUIRE(mismatched.has_error());
  REQUIRE(mismatched.error() == wh::core::errc::invalid_argument);

  auto rejected = wh::compose::apply_resume_decision(
      state, context,
      wh::compose::interrupt_resume_decision{
          .interrupt_context_id = "ctx",
          .decision = wh::compose::interrupt_decision_kind::reject,
      });
  REQUIRE(rejected.has_error());
  REQUIRE(rejected.error() == wh::core::errc::canceled);

  auto missing_edit_payload = wh::compose::apply_resume_decision(
      state, context,
      wh::compose::interrupt_resume_decision{
          .interrupt_context_id = "ctx",
          .decision = wh::compose::interrupt_decision_kind::edit,
      });
  REQUIRE(missing_edit_payload.has_error());
  REQUIRE(missing_edit_payload.error() == wh::core::errc::invalid_argument);

  auto edited = wh::compose::apply_resume_decision(
      state, context,
      wh::compose::interrupt_resume_decision{
          .interrupt_context_id = "ctx",
          .decision = wh::compose::interrupt_decision_kind::edit,
          .edited_payload = wh::core::any{9},
      });
  REQUIRE(edited.has_value());
  auto edited_payload = wh::compose::consume_resume_data<wh::compose::resume_patch>(
      state, "ctx");
  REQUIRE(edited_payload.has_value());
  REQUIRE(edited_payload.value().decision ==
          wh::compose::interrupt_decision_kind::edit);
  REQUIRE(*wh::core::any_cast<int>(&edited_payload.value().data) == 9);

  std::vector<wh::compose::resume_batch_item> items{
      {.interrupt_context_id = "missing", .data = wh::core::any{1}},
  };
  auto missing_contexts =
      wh::compose::apply_resume_batch(state, std::span{&context, 1}, items);
  REQUIRE(missing_contexts.has_error());
  REQUIRE(missing_contexts.error() == wh::core::errc::not_found);
}
