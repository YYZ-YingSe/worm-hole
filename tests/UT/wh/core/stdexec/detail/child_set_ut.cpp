#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <stdexcept>

#include <stdexec/execution.hpp>

#include "wh/core/stdexec/detail/child_set.hpp"

namespace {

struct child_sender_state {
  int starts{0};
  int destroys{0};
  int delivered{0};
};

struct child_receiver {
  using receiver_concept = stdexec::receiver_t;

  child_sender_state *state{nullptr};

  auto set_value(int value) && noexcept -> void { state->delivered = value; }
  template <typename error_t> auto set_error(error_t &&) && noexcept -> void {}
  auto set_stopped() && noexcept -> void {}
  [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
};

struct child_sender {
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(int)>;

  child_sender_state *state{nullptr};
  int value{0};

  template <typename receiver_t> struct operation {
    using operation_state_concept = stdexec::operation_state_t;

    child_sender_state *state{nullptr};
    receiver_t receiver;
    int value{0};

    auto start() & noexcept -> void {
      ++state->starts;
      stdexec::set_value(std::move(receiver), value);
    }

    ~operation() { ++state->destroys; }
  };

  template <typename receiver_t>
  auto connect(receiver_t receiver) const -> operation<receiver_t> {
    return operation<receiver_t>{state, std::move(receiver), value};
  }
};

} // namespace

TEST_CASE("child set starts children keeps ops engaged and reclaims owner-side",
          "[UT][wh/core/stdexec/detail/child_set.hpp][child_set::start_child][condition][branch]") {
  using child_op_t = stdexec::connect_result_t<child_sender, child_receiver>;

  child_sender_state left{};
  child_sender_state right{};
  wh::core::detail::child_set<child_op_t> children{2U};

  auto left_started = children.start_child(0U, [&](auto &slot) {
    slot.emplace_from(stdexec::connect, child_sender{&left, 7},
                      child_receiver{&left});
  });
  auto right_started = children.start_child(1U, [&](auto &slot) {
    slot.emplace_from(stdexec::connect, child_sender{&right, 9},
                      child_receiver{&right});
  });

  REQUIRE(left_started.has_value());
  REQUIRE(right_started.has_value());
  REQUIRE(children.active_count() == 2U);
  REQUIRE(left.starts == 1);
  REQUIRE(right.starts == 1);
  REQUIRE(left.delivered == 7);
  REQUIRE(right.delivered == 9);
  REQUIRE(left.destroys == 0);
  REQUIRE(right.destroys == 0);

  children.reclaim_child(0U);
  REQUIRE(children.active_count() == 1U);
  REQUIRE(left.destroys == 1);
  REQUIRE(right.destroys == 0);

  children.destroy_all();
  REQUIRE(children.active_count() == 0U);
  REQUIRE(right.destroys == 1);
}

TEST_CASE("child set converts start exceptions into result failure and leaves state clean",
          "[UT][wh/core/stdexec/detail/child_set.hpp][child_set::start_child][error][branch]") {
  using child_op_t = stdexec::connect_result_t<child_sender, child_receiver>;

  wh::core::detail::child_set<child_op_t> children{1U};
  auto started = children.start_child(0U, [](auto &) -> void {
    throw std::runtime_error{"boom"};
  });

  REQUIRE(started.has_error());
  REQUIRE(started.error() == wh::core::errc::internal_error);
  REQUIRE(children.active_count() == 0U);
}

TEST_CASE("child set reset clears engaged children and supports slot reuse",
          "[UT][wh/core/stdexec/detail/child_set.hpp][child_set::reset][condition][branch][boundary]") {
  using child_op_t = stdexec::connect_result_t<child_sender, child_receiver>;

  child_sender_state state{};
  wh::core::detail::child_set<child_op_t> children{1U};

  auto started = children.start_child(0U, [&](auto &slot) {
    slot.emplace_from(stdexec::connect, child_sender{&state, 3},
                      child_receiver{&state});
  });
  REQUIRE(started.has_value());
  REQUIRE(children.active_count() == 1U);
  REQUIRE(state.destroys == 0);

  children.reset(2U);
  REQUIRE(children.active_count() == 0U);
  REQUIRE(state.destroys == 1);

  auto reused = children.start_child(1U, [&](auto &slot) {
    slot.emplace_from(stdexec::connect, child_sender{&state, 5},
                      child_receiver{&state});
  });
  REQUIRE(reused.has_value());
  REQUIRE(children.active_count() == 1U);
  REQUIRE(state.delivered == 5);
}
