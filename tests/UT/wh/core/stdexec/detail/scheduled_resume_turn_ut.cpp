#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "helper/manual_scheduler.hpp"
#include "wh/core/stdexec/detail/scheduled_resume_turn.hpp"

namespace {

struct resume_turn_owner {
  wh::core::detail::scheduled_resume_turn<
      resume_turn_owner, wh::testing::helper::manual_scheduler<void>> *turn{
      nullptr};
  bool completed{false};
  bool nested_request_on_first_run{false};
  int finish_after{0};
  int run_calls{0};
  int idle_calls{0};
  int add_ref_calls{0};
  int arrive_calls{0};
  std::vector<wh::core::error_code> scheduled_errors{};

  [[nodiscard]] auto resume_turn_completed() const noexcept -> bool {
    return completed;
  }

  auto resume_turn_run() noexcept -> void {
    ++run_calls;
    if (nested_request_on_first_run && run_calls == 1 && turn != nullptr) {
      turn->request(this);
    }
    if (finish_after != 0 && run_calls >= finish_after) {
      completed = true;
    }
  }

  auto resume_turn_idle() noexcept -> void { ++idle_calls; }

  auto resume_turn_schedule_error(const wh::core::error_code error) noexcept
      -> void {
    scheduled_errors.push_back(error);
  }

  auto resume_turn_add_ref() noexcept -> void { ++add_ref_calls; }

  auto resume_turn_arrive() noexcept -> void { ++arrive_calls; }
};

} // namespace

TEST_CASE("scheduled_resume_turn coalesces repeated requests into one scheduled turn",
          "[UT][wh/core/stdexec/detail/scheduled_resume_turn.hpp][scheduled_resume_turn::request][branch][concurrency]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  wh::testing::helper::manual_scheduler<void> scheduler{&scheduler_state};
  wh::core::detail::scheduled_resume_turn<resume_turn_owner,
                                          decltype(scheduler)>
      turn{scheduler};
  resume_turn_owner owner{.turn = &turn};

  turn.request(&owner);
  turn.request(&owner);

  REQUIRE(scheduler_state.pending_count() == 1U);
  REQUIRE(owner.add_ref_calls == 1);
  REQUIRE_FALSE(turn.running());

  REQUIRE(scheduler_state.run_one());

  REQUIRE(owner.run_calls == 1);
  REQUIRE(owner.idle_calls == 1);
  REQUIRE(owner.arrive_calls == 1);
  REQUIRE(owner.scheduled_errors.empty());
  REQUIRE_FALSE(turn.running());
  REQUIRE(scheduler_state.pending_count() == 0U);
}

TEST_CASE("scheduled_resume_turn drains nested requests during an active turn before arriving",
          "[UT][wh/core/stdexec/detail/scheduled_resume_turn.hpp][scheduled_resume_turn::request][condition][branch][concurrency]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  wh::testing::helper::manual_scheduler<void> scheduler{&scheduler_state};
  wh::core::detail::scheduled_resume_turn<resume_turn_owner,
                                          decltype(scheduler)>
      turn{scheduler};
  resume_turn_owner owner{
      .turn = &turn,
      .nested_request_on_first_run = true,
      .finish_after = 2,
  };

  turn.request(&owner);

  REQUIRE(scheduler_state.pending_count() == 1U);
  REQUIRE(scheduler_state.run_one());

  REQUIRE(owner.run_calls == 2);
  REQUIRE(owner.completed);
  REQUIRE(owner.idle_calls == 0);
  REQUIRE(owner.add_ref_calls == 1);
  REQUIRE(owner.arrive_calls == 1);
  REQUIRE(owner.scheduled_errors.empty());
  REQUIRE_FALSE(turn.running());
  REQUIRE(scheduler_state.pending_count() == 0U);
}

TEST_CASE("scheduled_resume_turn ignores requests once the owner is already completed",
          "[UT][wh/core/stdexec/detail/scheduled_resume_turn.hpp][scheduled_resume_turn::request][boundary]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  wh::testing::helper::manual_scheduler<void> scheduler{&scheduler_state};
  wh::core::detail::scheduled_resume_turn<resume_turn_owner,
                                          decltype(scheduler)>
      turn{scheduler};
  resume_turn_owner owner{.turn = &turn, .completed = true};

  turn.request(&owner);

  REQUIRE(scheduler_state.pending_count() == 0U);
  REQUIRE(owner.run_calls == 0);
  REQUIRE(owner.add_ref_calls == 0);
  REQUIRE(owner.arrive_calls == 0);
  REQUIRE(owner.scheduled_errors.empty());
}
