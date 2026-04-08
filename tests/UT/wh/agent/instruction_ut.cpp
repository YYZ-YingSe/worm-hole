#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <vector>

#include "wh/agent/instruction.hpp"

static_assert(std::is_default_constructible_v<wh::agent::instruction>);

TEST_CASE("agent instruction starts empty and records append entries with stable sequence",
          "[UT][wh/agent/instruction.hpp][instruction::append][condition][branch][boundary]") {
  wh::agent::instruction instruction{};
  REQUIRE(instruction.empty());
  REQUIRE(instruction.entries().empty());
  REQUIRE(instruction.render().empty());

  instruction.append("system", 0);
  instruction.append("agent", 5);

  REQUIRE_FALSE(instruction.empty());
  REQUIRE(instruction.entries().size() == 2U);
  REQUIRE(instruction.entries()[0].mode == wh::agent::instruction_mode::append);
  REQUIRE(instruction.entries()[0].sequence == 0U);
  REQUIRE(instruction.entries()[1].sequence == 1U);
}

TEST_CASE("agent instruction render sorts append fragments by priority then sequence",
          "[UT][wh/agent/instruction.hpp][instruction::render][condition][branch][boundary]") {
  wh::agent::instruction instruction{};
  instruction.append("mid", 5);
  instruction.append("low", 0);
  instruction.append("high", 7);

  REQUIRE(instruction.render("|") == "low|mid|high");
}

TEST_CASE("agent instruction replace chooses highest-priority newest base and filters lower-priority appends",
          "[UT][wh/agent/instruction.hpp][instruction::replace][condition][branch][boundary]") {
  wh::agent::instruction instruction{};
  instruction.append("system", 0);
  instruction.replace("call", 10);
  instruction.append("ignored", 9);
  instruction.append("tail", 11);
  instruction.replace("call-2", 10);

  REQUIRE(instruction.render("|") == "call-2|tail");
}

TEST_CASE("agent instruction render skips empty fragments and honors custom separators",
          "[UT][wh/agent/instruction.hpp][instruction::render][condition][branch][boundary]") {
  wh::agent::instruction instruction{};
  instruction.append("", 0);
  instruction.replace("", 5);
  instruction.append("tail", 5);

  REQUIRE(instruction.render("|") == "tail");
}
