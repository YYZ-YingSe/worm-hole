#include <catch2/catch_test_macros.hpp>

#include "wh/core/stdexec/detail/callback_guard.hpp"

namespace {

struct callback_guard_owner {
  int exit_calls{0};

  auto on_callback_exit() noexcept -> void { ++exit_calls; }
};

} // namespace

TEST_CASE("callback guard tracks active depth and exits only after outermost scope",
          "[UT][wh/core/stdexec/detail/callback_guard.hpp][callback_guard::enter][condition][branch]") {
  wh::core::detail::callback_guard<callback_guard_owner> guard{};
  callback_guard_owner owner{};

  REQUIRE_FALSE(guard.active());

  {
    auto outer = guard.enter(&owner);
    REQUIRE(guard.active());
    REQUIRE(owner.exit_calls == 0);

    {
      auto inner = guard.enter(&owner);
      REQUIRE(guard.active());
      REQUIRE(owner.exit_calls == 0);
      static_cast<void>(inner);
    }

    REQUIRE(guard.active());
    REQUIRE(owner.exit_calls == 0);
    static_cast<void>(outer);
  }

  REQUIRE_FALSE(guard.active());
  REQUIRE(owner.exit_calls == 1);
}

TEST_CASE("callback guard scopes support move construction and move assignment",
          "[UT][wh/core/stdexec/detail/callback_guard.hpp][callback_guard::scope][branch][boundary]") {
  wh::core::detail::callback_guard<callback_guard_owner> guard{};
  callback_guard_owner owner{};

  {
    auto first = guard.enter(&owner);
    auto second = std::move(first);
    REQUIRE(guard.active());
    REQUIRE(owner.exit_calls == 0);

    wh::core::detail::callback_guard<callback_guard_owner>::scope assigned{};
    assigned = std::move(second);
    REQUIRE(guard.active());
    REQUIRE(owner.exit_calls == 0);
  }

  REQUIRE_FALSE(guard.active());
  REQUIRE(owner.exit_calls == 1);
}
