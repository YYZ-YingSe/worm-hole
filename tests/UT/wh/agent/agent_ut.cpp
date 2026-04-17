#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/agent.hpp"

TEST_CASE("agent shell exposes metadata topology transfer contracts and typed readers",
          "[UT][wh/agent/agent.hpp][agent::freeze][condition][branch][boundary]") {
  wh::agent::agent_transfer transfer{
      .target_agent_name = "planner",
      .tool_call_id = "call-1",
  };
  REQUIRE(transfer.target_agent_name == "planner");
  REQUIRE(transfer.tool_call_id == "call-1");

  wh::agent::agent_output output{};
  output.final_message = wh::testing::helper::make_text_message(
      wh::schema::message_role::assistant, "done");
  output.history_messages.push_back(wh::testing::helper::make_text_message(
      wh::schema::message_role::user, "input"));
  output.transfer = transfer;
  output.output_values.emplace("count", wh::core::any{3});
  REQUIRE(output.history_messages.size() == 1U);
  REQUIRE(output.transfer->target_agent_name == "planner");
  REQUIRE(*wh::core::any_cast<int>(&output.output_values.at("count")) == 3);

  wh::agent::output_reader<std::string> reader =
      [](const wh::agent::agent_output &value, wh::core::run_context &)
          -> wh::core::result<std::string> {
        return std::get<wh::schema::text_part>(value.final_message.parts.front())
            .text;
      };
  wh::core::run_context context{};
  auto read = reader(output, context);
  REQUIRE(read.has_value());
  REQUIRE(read.value() == "done");

  wh::agent::agent root{"root"};
  REQUIRE(root.name() == "root");
  REQUIRE_FALSE(root.parent_name().has_value());
  REQUIRE_FALSE(root.frozen());
  REQUIRE_FALSE(root.executable());
  REQUIRE(root.description().empty());

  REQUIRE(root.set_description("coordinator").has_value());
  REQUIRE(root.append_instruction("system", 1).has_value());
  REQUIRE(root.replace_instruction("override", 2).has_value());
  REQUIRE(root.render_instruction(" | ") == "override");
  REQUIRE(root.allow_transfer_to_parent().has_value());
  REQUIRE(root.allows_transfer_to_parent());

  REQUIRE(root.add_child(wh::agent::agent{"planner"}).has_value());
  REQUIRE(root.has_child("planner"));
  REQUIRE(root.child_count() == 1U);
  REQUIRE(root.child("planner").has_value());
  REQUIRE(root.child("planner").value().get().parent_name().has_value());
  REQUIRE(root.child("planner").value().get().parent_name().value() == "root");
  REQUIRE(root.child("missing").has_error());
  REQUIRE(root.child("missing").error() == wh::core::errc::not_found);

  REQUIRE(root.allow_transfer_to_child("planner").has_value());
  REQUIRE(root.allows_transfer_to_child("planner"));
  auto names = root.child_names();
  REQUIRE(names == std::vector<std::string>{"planner"});

  auto allowed = root.allowed_transfer_children();
  std::ranges::sort(allowed);
  REQUIRE(allowed == std::vector<std::string>{"planner"});
  REQUIRE(root.freeze().has_value());
  REQUIRE(root.frozen());
  REQUIRE(root.child("planner").value().get().frozen());
}

TEST_CASE("agent shell validates freeze lower and hook error paths",
          "[UT][wh/agent/agent.hpp][agent::lower][condition][branch][boundary]") {
  wh::agent::agent plain{"plain"};
  auto unfrozen = plain.lower();
  REQUIRE(unfrozen.has_error());
  REQUIRE(unfrozen.error() == wh::core::errc::contract_violation);
  REQUIRE(plain.freeze().has_value());
  auto unsupported = plain.lower();
  REQUIRE(unsupported.has_error());
  REQUIRE(unsupported.error() == wh::core::errc::not_supported);

  wh::agent::agent missing_transfer{"missing-transfer"};
  REQUIRE(missing_transfer.allow_transfer_to_child("ghost").has_value());
  auto missing_transfer_freeze = missing_transfer.freeze();
  REQUIRE(missing_transfer_freeze.has_error());
  REQUIRE(missing_transfer_freeze.error() == wh::core::errc::not_found);

  auto freeze_calls = std::make_shared<std::size_t>(0U);
  wh::agent::agent executable{"exec"};
  auto bound = executable.bind_execution(
      [freeze_calls]() -> wh::core::result<void> {
        ++*freeze_calls;
        return {};
      },
      []() -> wh::core::result<wh::compose::graph> {
        return wh::testing::helper::make_passthrough_graph("exec_node");
      });
  REQUIRE(bound.has_value());
  REQUIRE(executable.executable());
  REQUIRE(executable.freeze().has_value());
  REQUIRE(*freeze_calls == 1U);
  REQUIRE(executable.freeze().has_value());
  REQUIRE(*freeze_calls == 1U);

  auto graph = executable.lower();
  REQUIRE(graph.has_value());
  REQUIRE(graph.value().compiled());

  wh::agent::agent freeze_error{"freeze-error"};
  auto freeze_error_bound = freeze_error.bind_execution(
      []() -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::unavailable);
      },
      []() -> wh::core::result<wh::compose::graph> {
        return wh::testing::helper::make_passthrough_graph("freeze_error_node");
      });
  REQUIRE(freeze_error_bound.has_value());
  auto freeze_status = freeze_error.freeze();
  REQUIRE(freeze_status.has_error());
  REQUIRE(freeze_status.error() == wh::core::errc::unavailable);
}

TEST_CASE("agent shell rejects invalid mutations duplicate topology and missing lower hook",
          "[UT][wh/agent/agent.hpp][agent::bind_execution][condition][branch][boundary]") {
  wh::agent::agent root{"root"};
  REQUIRE(root.add_child(wh::agent::agent{""}).has_error());
  REQUIRE(root.add_child(wh::agent::agent{""}).error() ==
          wh::core::errc::invalid_argument);
  REQUIRE(root.add_child(wh::agent::agent{"planner"}).has_value());
  REQUIRE(root.add_child(wh::agent::agent{"planner"}).has_error());
  REQUIRE(root.add_child(wh::agent::agent{"planner"}).error() ==
          wh::core::errc::already_exists);
  REQUIRE(root.allow_transfer_to_child("").has_error());
  REQUIRE(root.allow_transfer_to_child("").error() ==
          wh::core::errc::invalid_argument);

  wh::agent::agent invalid_bind{"invalid-bind"};
  auto invalid = invalid_bind.bind_execution(nullptr, nullptr);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  REQUIRE(root.freeze().has_value());
  REQUIRE(root.set_description("late").has_error());
  REQUIRE(root.set_description("late").error() ==
          wh::core::errc::contract_violation);
  REQUIRE(root.append_instruction("late").has_error());
  REQUIRE(root.replace_instruction("late").has_error());
  REQUIRE(root.add_child(wh::agent::agent{"late"}).has_error());
  REQUIRE(root.allow_transfer_to_child("late").has_error());
  REQUIRE(root.allow_transfer_to_parent().has_error());
  REQUIRE(root.bind_execution(nullptr,
                              []() -> wh::core::result<wh::compose::graph> {
                                return wh::testing::helper::make_passthrough_graph(
                                    "late_node");
                              })
              .has_error());
  REQUIRE(root.bind_execution(nullptr,
                              []() -> wh::core::result<wh::compose::graph> {
                                return wh::testing::helper::make_passthrough_graph(
                                    "late_node");
                              })
              .error() == wh::core::errc::contract_violation);
}

TEST_CASE("agent shell bind_execution accepts lower hooks with copied and move-only captures",
          "[UT][wh/agent/agent.hpp][agent::bind_execution][condition][boundary]") {
  wh::agent::agent named{"named"};
  const std::string copied_name = "named_node";
  auto copied_bound = named.bind_execution(
      nullptr,
      [copied_name]() mutable -> wh::core::result<wh::compose::graph> {
        return wh::testing::helper::make_passthrough_graph(copied_name);
      });
  REQUIRE(copied_bound.has_value());
  REQUIRE(named.freeze().has_value());
  auto named_graph = named.lower();
  REQUIRE(named_graph.has_value());
  REQUIRE(named_graph->compiled());

  wh::agent::agent move_only{"move-only"};
  auto shell_name = std::make_unique<std::string>("move_only_node");
  auto move_only_bound = move_only.bind_execution(
      nullptr,
      [shell_name = std::move(shell_name)]() mutable
          -> wh::core::result<wh::compose::graph> {
        return wh::testing::helper::make_passthrough_graph(*shell_name);
      });
  REQUIRE(move_only_bound.has_value());
  REQUIRE(move_only.freeze().has_value());
  auto move_only_graph = move_only.lower();
  REQUIRE(move_only_graph.has_value());
  REQUIRE(move_only_graph->compiled());

  auto frozen_graph =
      wh::testing::helper::make_passthrough_graph("prebuilt_node");
  REQUIRE(frozen_graph.has_value());
  wh::agent::agent prebuilt{"prebuilt"};
  auto prebuilt_bound = prebuilt.bind_execution(
      nullptr,
      [graph = std::move(frozen_graph).value()]() mutable
          -> wh::core::result<wh::compose::graph> { return graph; });
  REQUIRE(prebuilt_bound.has_value());
  REQUIRE(prebuilt.freeze().has_value());
  auto prebuilt_graph = prebuilt.lower();
  REQUIRE(prebuilt_graph.has_value());
  REQUIRE(prebuilt_graph->compiled());
}
