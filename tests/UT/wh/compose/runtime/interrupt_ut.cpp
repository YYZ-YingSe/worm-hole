#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <vector>

#include "wh/compose/runtime/interrupt.hpp"

TEST_CASE("compose runtime interrupt helpers create ids and explicit signals",
          "[UT][wh/compose/runtime/interrupt.hpp][make_interrupt_signal][condition][branch][boundary]") {
  const auto first = wh::compose::make_interrupt_id();
  const auto second = wh::compose::make_interrupt_id();
  REQUIRE_FALSE(first.empty());
  REQUIRE_FALSE(second.empty());
  REQUIRE(first != second);

  const auto explicit_signal = wh::compose::make_interrupt_signal(
      "id-1", wh::core::make_address({"graph", "node"}), 7, std::string{"payload"});
  REQUIRE(explicit_signal.interrupt_id == "id-1");
  REQUIRE(explicit_signal.location.to_string() == "graph/node");
  REQUIRE(*wh::core::any_cast<int>(&explicit_signal.state) == 7);
  REQUIRE(*wh::core::any_cast<std::string>(&explicit_signal.layer_payload) ==
          "payload");
}

TEST_CASE("compose runtime interrupt helpers merge deduplicated sources",
          "[UT][wh/compose/runtime/interrupt.hpp][merge_interrupt_sources][condition][branch][boundary]") {
  std::vector<wh::core::interrupt_context> contexts{
      wh::core::interrupt_context{
          .interrupt_id = "a",
          .location = wh::core::make_address({"root", "a"}),
      },
  };
  std::vector<wh::core::interrupt_signal> signals{
      wh::core::interrupt_signal{
          .interrupt_id = "a",
          .location = wh::core::make_address({"sub", "a"}),
      },
      wh::core::interrupt_signal{
          .interrupt_id = "b",
          .location = wh::core::make_address({"sub", "b"}),
      },
  };

  const auto merged = wh::compose::merge_interrupt_sources(contexts, signals);
  REQUIRE(merged.size() == 2U);
  REQUIRE(merged[0].interrupt_id == "a");
  REQUIRE(merged[1].interrupt_id == "b");
}

TEST_CASE("compose runtime interrupt helpers cover auto-id projection tree conversion and reinterrupt mapping",
          "[UT][wh/compose/runtime/interrupt.hpp][to_reinterrupt_signal][condition][branch][boundary]") {
  auto auto_signal =
      wh::compose::make_interrupt_signal(wh::core::make_address({"graph", "leaf"}),
                                         9, std::string{"layer"});
  REQUIRE_FALSE(auto_signal.interrupt_id.empty());
  REQUIRE(auto_signal.location.to_string() == "graph/leaf");
  REQUIRE(*wh::core::any_cast<int>(&auto_signal.state) == 9);

  auto copied_context = wh::compose::to_interrupt_context(auto_signal);
  REQUIRE(copied_context.interrupt_id == auto_signal.interrupt_id);

  const std::array<std::string_view, 1U> filter{"leaf"};
  auto projected =
      wh::compose::project_interrupt_context(copied_context, std::span{filter});
  REQUIRE(projected.location.to_string() == "leaf");

  auto copied_signal = wh::compose::to_reinterrupt_signal(copied_context);
  REQUIRE(copied_signal.interrupt_id == auto_signal.interrupt_id);
  REQUIRE(*wh::core::any_cast<int>(&copied_signal.state) == 9);

  auto moved_signal = wh::compose::to_reinterrupt_signal(std::move(projected));
  REQUIRE(moved_signal.location.to_string() == "leaf");

  const std::vector<wh::core::interrupt_signal> signals{
      wh::compose::make_interrupt_signal(
          "root", wh::core::make_address({"root"}), 1),
      wh::compose::make_interrupt_signal(
          "child", wh::core::make_address({"root", "child"}), 2),
  };
  const auto snapshot =
      wh::compose::flatten_interrupt_signals(std::span{signals});
  REQUIRE(snapshot.interrupt_id_to_address.size() == 2U);

  const auto signal_tree =
      wh::compose::rebuild_interrupt_signal_tree(std::span{signals});
  REQUIRE(signal_tree.size() == 1U);
  const auto context_tree =
      wh::compose::to_interrupt_context_tree(std::span{signal_tree});
  REQUIRE(context_tree.size() == 1U);
  const auto rebuilt_signal_tree =
      wh::compose::to_interrupt_signal_tree(std::span{context_tree});
  REQUIRE(rebuilt_signal_tree.size() == 1U);
  const auto flattened =
      wh::compose::flatten_interrupt_signal_tree(std::span{rebuilt_signal_tree});
  REQUIRE(flattened.size() == 2U);

  std::vector<wh::core::interrupt_context> contexts{
      wh::core::interrupt_context{
          .interrupt_id = "",
          .location = wh::core::make_address({"skip"}),
      },
      copied_context,
  };
  std::vector<wh::core::interrupt_signal> duplicates{
      wh::compose::make_interrupt_signal(
          copied_context.interrupt_id, wh::core::make_address({"dup"}), 3),
      wh::compose::make_interrupt_signal(
          "other", wh::core::make_address({"other"}), 4),
  };
  const auto merged = wh::compose::merge_interrupt_sources(contexts, duplicates);
  REQUIRE(merged.size() == 2U);
  REQUIRE(merged[0].interrupt_id == copied_context.interrupt_id);
  REQUIRE(merged[1].interrupt_id == "other");
}
