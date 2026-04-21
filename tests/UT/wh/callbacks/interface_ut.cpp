#include <functional>
#include <memory>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/callbacks/interface.hpp"

namespace {

using timing_checker_t = decltype([](const wh::callbacks::stage) noexcept { return true; });
using stage_view_callback_t =
    decltype([](const wh::callbacks::stage, const wh::callbacks::event_view,
                const wh::callbacks::run_info &) {});
using stage_payload_callback_t =
    decltype([](const wh::callbacks::stage, wh::callbacks::event_payload &&,
                const wh::callbacks::run_info &) {});

static_assert(wh::callbacks::TimingChecker<timing_checker_t>);
static_assert(wh::callbacks::StageViewCallbackLike<stage_view_callback_t>);
static_assert(wh::callbacks::StagePayloadCallbackLike<stage_payload_callback_t>);

struct move_only_payload {
  int value{0};

  move_only_payload() = default;
  explicit move_only_payload(const int next) : value(next) {}
  move_only_payload(move_only_payload &&) noexcept = default;
  auto operator=(move_only_payload &&) noexcept -> move_only_payload & = default;
  move_only_payload(const move_only_payload &) = delete;
  auto operator=(const move_only_payload &) -> move_only_payload & = delete;
};

} // namespace

TEST_CASE("callbacks interface builds event views for mutable and const lvalues",
          "[UT][wh/callbacks/interface.hpp][make_event_view][condition][branch][boundary]") {
  int mutable_value = 7;
  const int const_value = 9;

  const auto mutable_view = wh::callbacks::make_event_view(mutable_value);
  const auto const_view = wh::callbacks::make_event_view(const_value);

  REQUIRE(mutable_view.get_if<int>() != nullptr);
  REQUIRE(*mutable_view.get_if<int>() == 7);
  REQUIRE(const_view.get_if<int>() != nullptr);
  REQUIRE(*const_view.get_if<int>() == 9);
}

TEST_CASE("callbacks interface payload accessors cover hit miss and reverse-stage checks",
          "[UT][wh/callbacks/interface.hpp][event_get_if][condition][branch][boundary]") {
  auto payload = wh::callbacks::make_event_payload(std::string{"hello"});

  REQUIRE(wh::callbacks::event_get_if<std::string>(payload) != nullptr);
  REQUIRE(wh::callbacks::event_get_if<int>(payload) == nullptr);
  REQUIRE(wh::callbacks::event_as<std::string>(payload).value() == "hello");
  REQUIRE(wh::callbacks::event_as<int>(payload).error() == wh::core::errc::type_mismatch);
  REQUIRE(wh::callbacks::event_cref_as<std::string>(payload).value().get() == "hello");
  REQUIRE(wh::callbacks::event_cref_as<int>(payload).error() == wh::core::errc::type_mismatch);
  REQUIRE(wh::callbacks::is_reverse_stage(wh::callbacks::stage::start));
  REQUIRE_FALSE(wh::callbacks::is_reverse_stage(wh::callbacks::stage::end));
  REQUIRE_FALSE(wh::callbacks::is_reverse_stage(wh::callbacks::stage::stream_end));
}

TEST_CASE("callbacks interface moves payloads out of rvalue event payload storage",
          "[UT][wh/callbacks/interface.hpp][event_as][condition][branch][boundary]") {
  auto payload = wh::callbacks::make_event_payload(move_only_payload{42});

  auto moved = wh::callbacks::event_as<move_only_payload>(std::move(payload));

  REQUIRE(moved.has_value());
  REQUIRE(moved->value == 42);
}
