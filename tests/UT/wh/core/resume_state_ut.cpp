#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/core/resume_state.hpp"

namespace {

[[nodiscard]] auto make_address(
    std::initializer_list<std::string_view> segments) -> wh::core::address {
  return wh::core::make_address(segments);
}

[[nodiscard]] auto make_signal(std::string id, wh::core::address location, int value,
                               std::string layer_payload = {},
                               bool used = false) -> wh::core::interrupt_signal {
  return wh::core::interrupt_signal{
      .interrupt_id = std::move(id),
      .location = std::move(location),
      .state = wh::core::any{value},
      .layer_payload = wh::core::any{std::move(layer_payload)},
      .used = used,
      .parent_locations = {make_address({"root"})},
      .trigger_reason = "manual",
  };
}

} // namespace

TEST_CASE("resume state payload conversion and projection preserve data",
          "[UT][wh/core/resume_state.hpp][to_interrupt_context][branch][boundary]") {
  const wh::core::interrupt_signal signal = make_signal(
      "sig-1", make_address({"root", "branch", "leaf"}), 7, "layer", true);

  auto copied_context = wh::core::to_interrupt_context(signal);
  REQUIRE(copied_context.interrupt_id == "sig-1");
  REQUIRE(copied_context.location == signal.location);
  REQUIRE(*wh::core::any_cast<int>(&copied_context.state) == 7);
  REQUIRE(*wh::core::any_cast<std::string>(&copied_context.layer_payload) ==
          "layer");
  REQUIRE(copied_context.used);
  REQUIRE(copied_context.parent_locations == signal.parent_locations);
  REQUIRE(copied_context.trigger_reason == "manual");

  auto moved_context = wh::core::to_interrupt_context(make_signal(
      "sig-2", make_address({"root", "other"}), 9, "payload", false));
  REQUIRE(moved_context.interrupt_id == "sig-2");
  REQUIRE(*wh::core::any_cast<int>(&moved_context.state) == 9);

  auto copied_signal = wh::core::to_interrupt_signal(copied_context);
  REQUIRE(copied_signal.interrupt_id == copied_context.interrupt_id);
  REQUIRE(*wh::core::any_cast<int>(&copied_signal.state) == 7);

  auto moved_signal = wh::core::to_interrupt_signal(std::move(moved_context));
  REQUIRE(moved_signal.interrupt_id == "sig-2");
  REQUIRE(*wh::core::any_cast<int>(&moved_signal.state) == 9);

  auto cloned = wh::core::clone_interrupt_payload_any(signal.state);
  REQUIRE(*wh::core::any_cast<int>(&cloned) == 7);

  const std::array<std::string_view, 2U> filter{"root", "leaf"};
  auto projected = wh::core::project_address(signal.location, filter);
  REQUIRE(projected == make_address({"root", "leaf"}));
  REQUIRE(wh::core::project_address(signal.location, {}).to_string() ==
          signal.location.to_string());

  auto projected_copy = wh::core::project_interrupt_context(copied_context, filter);
  REQUIRE(projected_copy.location == make_address({"root", "leaf"}));
  REQUIRE(projected_copy.interrupt_id == copied_context.interrupt_id);

  auto projected_move = wh::core::project_interrupt_context(
      wh::core::interrupt_context{
          .interrupt_id = "ctx",
          .location = make_address({"a", "b", "c"}),
          .state = wh::core::any{3},
      },
      std::span<const std::string_view>{});
  REQUIRE(projected_move.location == make_address({"a", "b", "c"}));
}

TEST_CASE("resume state tree rebuild conversion and flatten keep hierarchy",
          "[UT][wh/core/resume_state.hpp][rebuild_interrupt_signal_tree][branch][boundary]") {
  const std::vector<wh::core::interrupt_signal> signals{
      make_signal("root-signal", make_address({"root"}), 1),
      make_signal("child-a", make_address({"root", "a"}), 2),
      make_signal("child-b", make_address({"root", "b"}), 3),
  };

  const auto signal_tree =
      wh::core::rebuild_interrupt_signal_tree(std::span{signals});
  REQUIRE(signal_tree.size() == 1U);
  REQUIRE(signal_tree.front().location == make_address({"root"}));
  REQUIRE(signal_tree.front().signals.size() == 1U);
  REQUIRE(signal_tree.front().children.size() == 2U);

  const auto flattened_signals =
      wh::core::flatten_interrupt_signal_tree(std::span{signal_tree});
  REQUIRE(flattened_signals.size() == 3U);

  const auto context_tree = wh::core::to_interrupt_context_tree(std::span{signal_tree});
  REQUIRE(context_tree.size() == 1U);
  REQUIRE(context_tree.front().contexts.size() == 1U);
  REQUIRE(context_tree.front().children.size() == 2U);

  const auto flattened_contexts =
      wh::core::flatten_interrupt_context_tree(std::span{context_tree});
  REQUIRE(flattened_contexts.size() == 3U);

  const auto rebuilt_context_tree =
      wh::core::rebuild_interrupt_context_tree(std::span{flattened_contexts});
  REQUIRE(rebuilt_context_tree.size() == 1U);
  REQUIRE(rebuilt_context_tree.front().children.size() == 2U);

  const auto rebuilt_signal_tree =
      wh::core::to_interrupt_signal_tree(std::span{rebuilt_context_tree});
  REQUIRE(rebuilt_signal_tree.size() == 1U);
  REQUIRE(rebuilt_signal_tree.front().children.size() == 2U);
}

TEST_CASE("resume state flatten interrupt signals supports copy and move snapshots",
          "[UT][wh/core/resume_state.hpp][flatten_interrupt_signals][branch][boundary]") {
  const std::vector<wh::core::interrupt_signal> copy_source{
      make_signal("a", make_address({"root", "a"}), 11),
      make_signal("b", make_address({"root", "b"}), 22),
  };

  const auto copied_snapshot =
      wh::core::flatten_interrupt_signals(std::span{copy_source});
  REQUIRE(copied_snapshot.interrupt_id_to_address.size() == 2U);
  REQUIRE(copied_snapshot.interrupt_id_to_address.at("a") ==
          make_address({"root", "a"}));
  REQUIRE(*wh::core::any_cast<int>(
              &copied_snapshot.interrupt_id_to_state.at("b")) == 22);

  auto move_source = copy_source;
  const auto moved_snapshot =
      wh::core::flatten_interrupt_signals(std::move(move_source));
  REQUIRE(moved_snapshot.interrupt_id_to_state.size() == 2U);
  REQUIRE(*wh::core::any_cast<int>(
              &moved_snapshot.interrupt_id_to_state.at("a")) == 11);
}

TEST_CASE("resume state upsert merge and lookup cover replace and self-merge branches",
          "[UT][wh/core/resume_state.hpp][resume_state::upsert][condition][branch][boundary]") {
  wh::core::resume_state state{};

  auto invalid =
      state.upsert("", make_address({"root", "a"}), wh::core::any{1});
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  REQUIRE(state.upsert("id-a", make_address({"root", "a"}), 1).has_value());
  REQUIRE(state.upsert("id-b", make_address({"root", "b"}), 2).has_value());
  REQUIRE(state.upsert("id-c", make_address({"root", "a", "leaf"}), 3).has_value());
  REQUIRE(state.size() == 3U);
  REQUIRE_FALSE(state.empty());
  REQUIRE(state.contains_interrupt_id("id-a"));
  REQUIRE_FALSE(state.contains_interrupt_id("missing"));

  const auto ids = state.interrupt_ids();
  REQUIRE(ids == std::vector<std::string>{"id-a", "id-b", "id-c"});

  auto location = state.location_of("id-b");
  REQUIRE(location.has_value());
  REQUIRE(location.value().get() == make_address({"root", "b"}));

  auto missing_location = state.location_of("ghost");
  REQUIRE(missing_location.has_error());
  REQUIRE(missing_location.error() == wh::core::errc::not_found);

  REQUIRE(state.is_resume_target(make_address({})));
  REQUIRE(state.is_resume_target(make_address({"root"})));
  REQUIRE(state.is_exact_resume_target(make_address({"root", "a"})));
  REQUIRE_FALSE(state.is_exact_resume_target(make_address({"root"})));

  const auto next_points = state.next_resume_points(make_address({"root"}));
  REQUIRE(next_points == std::vector<std::string>{"a", "b"});

  REQUIRE(state.merge(state).has_value());
  REQUIRE(state.size() == 3U);

  wh::core::resume_state delta{};
  REQUIRE(delta.upsert("id-a", make_address({"override"}), 7).has_value());
  REQUIRE(delta.upsert("id-d", make_address({"root", "d"}), 8).has_value());
  REQUIRE(state.merge(delta).has_value());
  REQUIRE(state.size() == 4U);
  REQUIRE(state.location_of("id-a").value().get() == make_address({"override"}));

  wh::core::resume_state moved_delta{};
  REQUIRE(moved_delta.upsert("id-e", make_address({"root", "e"}), 9).has_value());
  REQUIRE(state.merge(std::move(moved_delta)).has_value());
  REQUIRE(state.contains_interrupt_id("id-e"));
  REQUIRE(moved_delta.empty());
}

TEST_CASE("resume state subtree queries used flags and erase options cover active bookkeeping",
          "[UT][wh/core/resume_state.hpp][resume_state::collect_subtree_interrupt_ids][condition][branch][boundary]") {
  wh::core::resume_state state{};
  REQUIRE(state.upsert("id-a", make_address({"root", "a"}), 1).has_value());
  REQUIRE(state.upsert("id-b", make_address({"root", "a", "leaf"}), 2).has_value());
  REQUIRE(state.upsert("id-c", make_address({"root", "b"}), 3).has_value());

  const auto collected_before =
      state.collect_subtree_interrupt_ids(make_address({"root", "a"}));
  REQUIRE(collected_before == std::vector<std::string>{"id-a", "id-b"});

  REQUIRE(state.mark_subtree_used(make_address({"root", "a"})) == 2U);
  REQUIRE(state.is_used("id-a"));
  REQUIRE(state.is_used("id-b"));
  REQUIRE_FALSE(state.is_used("id-c"));
  REQUIRE_FALSE(state.is_resume_target(make_address({"root", "a"})));
  REQUIRE(state.is_resume_target(make_address({"root"})));

  const auto active_only =
      state.collect_subtree_interrupt_ids(make_address({"root", "a"}));
  REQUIRE(active_only.empty());

  const auto include_used = state.collect_subtree_interrupt_ids(
      make_address({"root", "a"}),
      wh::core::resume_subtree_query_options{.include_used = true});
  REQUIRE(include_used == std::vector<std::string>{"id-a", "id-b"});

  REQUIRE(state.erase_subtree(
              make_address({"root", "a"}),
              wh::core::resume_subtree_erase_options{.include_used = false}) == 0U);
  REQUIRE(state.contains_interrupt_id("id-a"));

  REQUIRE(state.erase_subtree(
              make_address({"root", "a"}),
              wh::core::resume_subtree_erase_options{.include_used = true}) == 2U);
  REQUIRE_FALSE(state.contains_interrupt_id("id-a"));
  REQUIRE_FALSE(state.contains_interrupt_id("id-b"));
  REQUIRE(state.contains_interrupt_id("id-c"));
}

TEST_CASE("resume state consume peek and mark_used surface error branches",
          "[UT][wh/core/resume_state.hpp][resume_state::consume][condition][branch][boundary]") {
  wh::core::resume_state state{};
  REQUIRE(state.upsert("value", make_address({"root", "value"}), 7).has_value());
  REQUIRE(state.upsert("text", make_address({"root", "text"}), std::string{"abc"})
              .has_value());

  const auto peek_text = state.peek<std::string>("text");
  REQUIRE(peek_text.has_value());
  REQUIRE(peek_text->get() == "abc");

  const auto peek_missing = state.peek<int>("ghost");
  REQUIRE(peek_missing.has_error());
  REQUIRE(peek_missing.error() == wh::core::errc::not_found);

  const auto peek_mismatch = state.peek<int>("text");
  REQUIRE(peek_mismatch.has_error());
  REQUIRE(peek_mismatch.error() == wh::core::errc::type_mismatch);

  const auto consumed = state.consume<int>("value");
  REQUIRE(consumed.has_value());
  REQUIRE(consumed.value() == 7);
  REQUIRE(state.is_used("value"));

  const auto consumed_again = state.consume<int>("value");
  REQUIRE(consumed_again.has_error());
  REQUIRE(consumed_again.error() == wh::core::errc::contract_violation);

  const auto consume_mismatch = state.consume<int>("text");
  REQUIRE(consume_mismatch.has_error());
  REQUIRE(consume_mismatch.error() == wh::core::errc::type_mismatch);

  const auto consume_missing = state.consume<int>("missing");
  REQUIRE(consume_missing.has_error());
  REQUIRE(consume_missing.error() == wh::core::errc::not_found);

  auto mark_text = state.mark_used("text");
  REQUIRE(mark_text.has_value());
  REQUIRE(state.is_used("text"));

  const auto mark_text_again = state.mark_used("text");
  REQUIRE(mark_text_again.has_error());
  REQUIRE(mark_text_again.error() == wh::core::errc::contract_violation);

  const auto mark_missing = state.mark_used("missing");
  REQUIRE(mark_missing.has_error());
  REQUIRE(mark_missing.error() == wh::core::errc::not_found);

  REQUIRE(state.interrupt_ids(false).empty());
}
