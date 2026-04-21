#include <atomic>
#include <optional>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/reader/detail/exclusive_owner_gate.hpp"

TEST_CASE(
    "exclusive owner gate claims phases releases and token matching enforce single-owner contract",
    "[UT][wh/schema/stream/reader/detail/"
    "exclusive_owner_gate.hpp][exclusive_owner_gate::try_claim][condition][branch][boundary]") {
  wh::schema::stream::detail::exclusive_owner_gate gate{};

  REQUIRE_FALSE(gate.claimed());
  REQUIRE(gate.current_kind() == wh::schema::stream::detail::owner_kind::none);
  REQUIRE(gate.current_phase() == wh::schema::stream::detail::owner_phase::idle);
  REQUIRE(gate.current_token() == 0U);

  auto claim = gate.try_claim(wh::schema::stream::detail::owner_kind::sync);
  REQUIRE(claim.has_value());
  REQUIRE(claim->kind == wh::schema::stream::detail::owner_kind::sync);
  REQUIRE(claim->token == 1U);
  REQUIRE(gate.claimed());
  REQUIRE(gate.matches(claim->token));
  REQUIRE_FALSE(gate.try_claim(wh::schema::stream::detail::owner_kind::async).has_value());

  REQUIRE(gate.set_phase(claim->token, wh::schema::stream::detail::owner_phase::topology_wait));
  REQUIRE(gate.current_phase() == wh::schema::stream::detail::owner_phase::topology_wait);
  REQUIRE_FALSE(
      gate.set_phase(claim->token + 1U, wh::schema::stream::detail::owner_phase::round_active));

  REQUIRE(gate.release(claim->token));
  REQUIRE_FALSE(gate.claimed());
  REQUIRE_FALSE(gate.release(claim->token));
  REQUIRE_FALSE(gate.matches(claim->token));

  auto next = gate.try_claim(wh::schema::stream::detail::owner_kind::async);
  REQUIRE(next.has_value());
  REQUIRE(next->token == 2U);
  REQUIRE(next->kind == wh::schema::stream::detail::owner_kind::async);
  REQUIRE(gate.current_kind() == wh::schema::stream::detail::owner_kind::async);
}

TEST_CASE("exclusive owner gate rejects stale tokens without disturbing active owner",
          "[UT][wh/schema/stream/reader/detail/"
          "exclusive_owner_gate.hpp][exclusive_owner_gate::release][condition][branch][boundary]") {
  wh::schema::stream::detail::exclusive_owner_gate gate{};
  auto claim = gate.try_claim(wh::schema::stream::detail::owner_kind::sync);
  REQUIRE(claim.has_value());

  REQUIRE_FALSE(gate.release(claim->token + 1U));
  REQUIRE(gate.claimed());
  REQUIRE(gate.matches(claim->token));
  REQUIRE(gate.current_phase() == wh::schema::stream::detail::owner_phase::idle);

  REQUIRE(gate.set_phase(claim->token, wh::schema::stream::detail::owner_phase::finishing));
  REQUIRE(gate.current_phase() == wh::schema::stream::detail::owner_phase::finishing);
  REQUIRE(gate.release(claim->token));
  REQUIRE_FALSE(gate.claimed());
}

TEST_CASE("exclusive owner gate allows only one concurrent claimant at a time",
          "[UT][wh/schema/stream/reader/detail/"
          "exclusive_owner_gate.hpp][exclusive_owner_gate::try_claim][concurrency][branch]") {
  wh::schema::stream::detail::exclusive_owner_gate gate{};
  std::atomic<bool> start{false};
  std::optional<wh::schema::stream::detail::exclusive_owner_gate::claim> first{};
  std::optional<wh::schema::stream::detail::exclusive_owner_gate::claim> second{};

  auto claimant = [&](std::optional<wh::schema::stream::detail::exclusive_owner_gate::claim> *slot,
                      const wh::schema::stream::detail::owner_kind kind) {
    while (!start.load(std::memory_order_acquire)) {
    }
    *slot = gate.try_claim(kind);
  };

  std::thread left([&] { claimant(&first, wh::schema::stream::detail::owner_kind::sync); });
  std::thread right([&] { claimant(&second, wh::schema::stream::detail::owner_kind::async); });
  start.store(true, std::memory_order_release);
  left.join();
  right.join();

  const auto success_count =
      static_cast<int>(first.has_value()) + static_cast<int>(second.has_value());
  REQUIRE(success_count == 1);

  const auto winner = first.has_value() ? first.value() : second.value();
  REQUIRE(gate.claimed());
  REQUIRE(gate.matches(winner.token));
  REQUIRE(gate.release(winner.token));
  REQUIRE_FALSE(gate.claimed());
}
