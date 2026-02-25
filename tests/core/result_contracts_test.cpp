#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "wh/core/result.hpp"
#include "wh/core/type_utils.hpp"

namespace {

struct pseudo_result_like {
  using value_type = int;
  using error_type = wh::core::errc;

  explicit pseudo_result_like(const int value) : payload(value) {}
  explicit pseudo_result_like(const wh::core::errc) : payload(-1) {}

  int payload;
};

static_assert(!wh::core::detail::result_like<pseudo_result_like>);
static_assert(wh::core::detail::result_like<wh::core::result<int>>);

} // namespace

TEST_CASE("result success and failure value access",
          "[core][result][condition]") {
  const auto success = wh::core::result<int>{wh::core::success(42)};
  REQUIRE(success.has_value());
  REQUIRE_FALSE(success.has_error());
  REQUIRE(success.value() == 42);
  REQUIRE(success.value_or(0) == 42);

  const auto failure = wh::core::result<int>{
      wh::core::failure(wh::core::errc::invalid_argument)};
  REQUIRE_FALSE(failure.has_value());
  REQUIRE(failure.has_error());
  REQUIRE(failure.error() == wh::core::errc::invalid_argument);
  REQUIRE(failure.value_or(-1) == -1);
}

TEST_CASE("result boost-style in-place and implicit constructors",
          "[core][result][branch]") {
  const wh::core::result<int> implicit_value = 7;
  REQUIRE(implicit_value.has_value());
  REQUIRE(implicit_value.value() == 7);

  const wh::core::result<int> tagged_value{wh::core::in_place_value, 9};
  REQUIRE(tagged_value.has_value());
  REQUIRE(tagged_value.value() == 9);

  const wh::core::result<int> tagged_error{wh::core::in_place_error,
                                           wh::core::errc::timeout};
  REQUIRE(tagged_error.has_error());
  REQUIRE(tagged_error.error() == wh::core::errc::timeout);

  const wh::core::result<short> narrow_value = static_cast<short>(3);
  const wh::core::result<int> widened_value{narrow_value};
  REQUIRE(widened_value.has_value());
  REQUIRE(widened_value.value() == 3);
}

TEST_CASE("result observers operator and assume contract",
          "[core][result][branch]") {
  auto text = wh::core::result<std::string>::success("alpha");
  REQUIRE((*text) == "alpha");
  REQUIRE(text->size() == 5U);

  auto moved = wh::core::result<std::string>::success("beta");
  REQUIRE(std::move(moved).assume_value() == "beta");

  auto failed =
      wh::core::result<std::string>{wh::core::failure(wh::core::errc::timeout)};
  REQUIRE(failed.assume_error() == wh::core::errc::timeout);
  REQUIRE(failed.operator->() == nullptr);
}

TEST_CASE("result emplace swap equality and stream output",
          "[core][result][branch]") {
  wh::core::result<std::string> left{"left"};
  wh::core::result<std::string> right{wh::core::errc::timeout};

  left.emplace("updated");
  REQUIRE(left.has_value());
  REQUIRE(*left == "updated");

  left.swap(right);
  REQUIRE(left.has_error());
  REQUIRE(right.has_value());

  swap(left, right);
  REQUIRE(left.has_value());
  REQUIRE(right.has_error());

  const wh::core::result<std::string> same{"updated"};
  REQUIRE(left == same);
  REQUIRE(left != right);

  std::ostringstream oss;
  oss << left;
  REQUIRE(oss.str() == "value:updated");
}

TEST_CASE("result reference specialization semantics",
          "[core][result][condition]") {
  int source = 17;
  wh::core::result<int &> ref_result{source};
  REQUIRE(ref_result.has_value());
  REQUIRE(&ref_result.value() == &source);
  REQUIRE(ref_result.operator->() == &source);

  *ref_result = 21;
  REQUIRE(source == 21);

  int fallback = 9;
  REQUIRE(&ref_result.value_or(fallback) == &source);

  long wide_source = 33;
  const wh::core::result<long &> ref_long{wide_source};
  wh::core::result<const long &> converted{ref_long};
  REQUIRE(converted.has_value());
  REQUIRE(&converted.value() == &wide_source);

  const wh::core::result<int &> failed{wh::core::errc::queue_empty};
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::queue_empty);

  ref_result.emplace(source);
  REQUIRE(ref_result.has_value());
}

TEST_CASE("result copy move and move-only payload semantics",
          "[core][result][branch]") {
  const auto original = wh::core::result<std::string>::success("alpha");
  const auto copied = original;
  REQUIRE(copied.has_value());
  REQUIRE(copied.value() == "alpha");

  auto moved_error =
      wh::core::result<std::string>{wh::core::failure(wh::core::errc::timeout)};
  auto moved_target = std::move(moved_error);
  REQUIRE(moved_target.has_error());
  REQUIRE(moved_target.error() == wh::core::errc::timeout);

  const wh::core::result<std::string> no_error{"safe"};
  REQUIRE(no_error.error() == wh::core::errc::ok);

  auto move_only =
      wh::core::result<std::unique_ptr<int>>::success(std::make_unique<int>(7));
  REQUIRE(move_only.has_value());
  REQUIRE(*move_only.value() == 7);

  auto rvalue_value = wh::core::result<std::string>::success("beta");
  REQUIRE(std::move(rvalue_value).value_or(std::string{"fallback"}) == "beta");
}

TEST_CASE("result void specialization and errc boundaries",
          "[core][result][extreme]") {
  const auto ok = wh::core::result<void>::success();
  REQUIRE(ok.has_value());
  REQUIRE_FALSE(ok.has_error());
  ok.value();

  const auto ok_sugar = wh::core::result<void>{wh::core::success()};
  REQUIRE(ok_sugar.has_value());

  const auto failed =
      wh::core::result<void>{wh::core::failure(wh::core::errc::canceled)};
  REQUIRE_FALSE(failed.has_value());
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::canceled);
  REQUIRE(failed.assume_error() == wh::core::errc::canceled);

  const wh::core::result<void> implicit_error = wh::core::errc::queue_full;
  REQUIRE(implicit_error.has_error());
  REQUIRE(implicit_error.error() == wh::core::errc::queue_full);

  auto transient = wh::core::result<void>{wh::core::errc::timeout};
  transient.emplace();
  REQUIRE(transient.has_value());

  REQUIRE(implicit_error.operator->() == nullptr);

  std::ostringstream oss;
  oss << ok;
  REQUIRE(oss.str() == "value:void");

  REQUIRE(wh::core::to_string(wh::core::errc::ok) == "ok");
  REQUIRE(wh::core::to_string(wh::core::errc::scheduler_not_bound) ==
          "scheduler_not_bound");
  REQUIRE(wh::core::to_string(wh::core::errc::queue_full) == "queue_full");

  static_assert(wh::core::is_result_v<wh::core::result<int>>);
  static_assert(wh::core::is_result_v<wh::core::result<void>>);
  static_assert(wh::core::is_result_v<wh::core::result<int &>>);
  static_assert(
      std::same_as<wh::core::result<int>::error_type, wh::core::error_code>);
}

TEST_CASE("result pipe fallback operators", "[core][result][branch]") {
  const wh::core::result<int> ok_value{4};
  const wh::core::result<int> err_value{wh::core::errc::timeout};

  REQUIRE((ok_value | 11) == 4);
  REQUIRE((err_value | 11) == 11);

  REQUIRE((ok_value | [] { return 13; }) == 4);
  REQUIRE((err_value | [] { return 13; }) == 13);

  const auto result_fallback =
      ok_value | [] { return wh::core::result<long>{99}; };
  REQUIRE(result_fallback.has_value());
  REQUIRE(result_fallback.value() == 4);

  const auto result_from_error =
      err_value | [] { return wh::core::result<int>{42}; };
  REQUIRE(result_from_error.has_value());
  REQUIRE(result_from_error.value() == 42);

  const wh::core::result<void> ok_void{};
  const wh::core::result<void> err_void{wh::core::errc::queue_empty};

  const auto void_fallback_from_ok =
      ok_void | [] { return wh::core::result<void>{wh::core::errc::timeout}; };
  REQUIRE(void_fallback_from_ok.has_value());

  const auto void_fallback_from_err =
      err_void | [] { return wh::core::result<void>{}; };
  REQUIRE(void_fallback_from_err.has_value());
}

TEST_CASE("result and-chain operators", "[core][result][condition]") {
  const wh::core::result<int> ok_value{5};
  const wh::core::result<int> err_value{wh::core::errc::canceled};

  const auto mapped = ok_value & [](const int value) { return value * 2; };
  REQUIRE(mapped.has_value());
  REQUIRE(mapped.value() == 10);

  const auto mapped_error =
      err_value & [](const int value) { return value * 2; };
  REQUIRE(mapped_error.has_error());
  REQUIRE(mapped_error.error() == wh::core::errc::canceled);

  const auto as_result = ok_value & [](const int value) {
    return wh::core::result<std::string>{std::to_string(value)};
  };
  REQUIRE(as_result.has_value());
  REQUIRE(as_result.value() == "5");

  const wh::core::result<void> ok_void{};
  const auto from_void = ok_void & [] { return 7; };
  REQUIRE(from_void.has_value());
  REQUIRE(from_void.value() == 7);
}

TEST_CASE("result inplace chain update operators",
          "[core][result][condition]") {
  wh::core::result<int> recover{wh::core::errc::timeout};
  recover |= 12;
  REQUIRE(recover.has_value());
  REQUIRE(recover.value() == 12);

  wh::core::result<int> recover_with_factory{wh::core::errc::canceled};
  recover_with_factory |= [] { return 23; };
  REQUIRE(recover_with_factory.has_value());
  REQUIRE(recover_with_factory.value() == 23);

  wh::core::result<int> recover_with_result{wh::core::errc::queue_empty};
  recover_with_result |= [] { return wh::core::result<int>{31}; };
  REQUIRE(recover_with_result.has_value());
  REQUIRE(recover_with_result.value() == 31);

  wh::core::result<int> map_ok{3};
  map_ok &= [](const int value) { return value + 4; };
  REQUIRE(map_ok.has_value());
  REQUIRE(map_ok.value() == 7);

  wh::core::result<int> map_ok_result{8};
  map_ok_result &=
      [](const int value) { return wh::core::result<int>{value * 2}; };
  REQUIRE(map_ok_result.has_value());
  REQUIRE(map_ok_result.value() == 16);

  wh::core::result<void> void_ok{};
  bool called = false;
  void_ok &= [&called] { called = true; };
  REQUIRE(called);

  wh::core::result<void> void_ok_result{};
  void_ok_result &= [] { return wh::core::result<void>{}; };
  REQUIRE(void_ok_result.has_value());
}
