#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "wh/core/result.hpp"

namespace {

struct pseudo_result_like {
  using value_type = int;
  using error_type = wh::core::errc;

  explicit pseudo_result_like(int value) : payload(value) {}
  explicit pseudo_result_like(wh::core::errc) : payload(-1) {}

  int payload{0};
};

struct move_only_box {
  explicit move_only_box(int next) : value(std::make_unique<int>(next)) {}

  move_only_box(move_only_box &&) noexcept = default;
  auto operator=(move_only_box &&) noexcept -> move_only_box & = default;

  move_only_box(const move_only_box &) = delete;
  auto operator=(const move_only_box &) -> move_only_box & = delete;

  [[nodiscard]] auto operator==(const move_only_box &other) const -> bool {
    return *value == *other.value;
  }

  std::unique_ptr<int> value{};
};

using wh::core::detail::callable_result_t;
using wh::core::detail::callable_with;
using wh::core::detail::remove_cvref_t;
using wh::core::errc;
using wh::core::failure;
using wh::core::in_place_error;
using wh::core::in_place_value;
using wh::core::result;
using wh::core::success;

static_assert(std::same_as<remove_cvref_t<const int &>, int>);
static_assert(callable_with<decltype([](int value) { return value + 1; }), int>);
static_assert(std::same_as<
              callable_result_t<decltype([](int value) { return value + 1; }),
                                int>,
              int>);
static_assert(wh::core::detail::is_result<result<int>>::value);
static_assert(wh::core::detail::result_like<result<int>>);
static_assert(!wh::core::detail::result_like<pseudo_result_like>);
static_assert(std::same_as<result<int>::error_type, wh::core::error_code>);
static_assert(!std::constructible_from<result<int &, errc>, int>);

} // namespace

TEST_CASE("success and failure tags preserve payload access",
          "[UT][wh/core/result.hpp][success][branch][boundary]") {
  auto ok = success(std::string{"alpha"});
  static_assert(std::same_as<decltype(ok.value()), std::string &>);
  static_assert(std::same_as<decltype(std::move(ok).value()), std::string &&>);
  REQUIRE(ok.value() == "alpha");

  const auto copied_ok = success(42);
  REQUIRE(copied_ok.value() == 42);

  auto failed = failure(errc::timeout);
  REQUIRE(failed.error() == errc::timeout);

  const auto void_ok = success();
  static_assert(
      std::same_as<decltype(void_ok), const wh::core::success_type<void>>);
}

TEST_CASE("result value specialization covers constructors observers and stream",
          "[UT][wh/core/result.hpp][result<T>][branch][boundary]") {
  const result<int> implicit_value = 7;
  const result<int> tagged_value{in_place_value, 9};
  const result<int> tagged_error{in_place_error, errc::timeout};
  const result<int> from_success{success(11)};
  const result<int> from_failure{failure(errc::queue_empty)};

  REQUIRE(implicit_value.has_value());
  REQUIRE(tagged_value.has_value());
  REQUIRE(tagged_error.has_error());
  REQUIRE(from_success.value() == 11);
  REQUIRE(from_failure.error() == errc::queue_empty);
  REQUIRE(tagged_value.value() == 9);
  REQUIRE(tagged_error.error() == errc::timeout);

  auto text = result<std::string>::success("alpha");
  REQUIRE((*text) == "alpha");
  REQUIRE(text->size() == 5U);
  REQUIRE(text.assume_value() == "alpha");

  auto failed_text = result<std::string>{failure(errc::timeout)};
  REQUIRE(failed_text.assume_error() == errc::timeout);
  REQUIRE(failed_text.operator->() == nullptr);
  REQUIRE(failed_text.value_or(std::string{"fallback"}) == "fallback");

  text.emplace("beta");
  REQUIRE(text.value() == "beta");

  result<std::string> swapped_error{errc::not_found};
  text.swap(swapped_error);
  REQUIRE(text.has_error());
  REQUIRE(swapped_error.has_value());

  swap(text, swapped_error);
  REQUIRE(text.has_value());
  REQUIRE(swapped_error.has_error());
  REQUIRE(text == result<std::string>{"beta"});
  REQUIRE(text != swapped_error);

  std::ostringstream stream{};
  stream << text;
  REQUIRE(stream.str() == "value:beta");
}

TEST_CASE("result value specialization supports conversion and move-only payloads",
          "[UT][wh/core/result.hpp][result<T>::value_or][branch][boundary]") {
  const result<short> narrow_value = static_cast<short>(3);
  const result<int> widened_value{narrow_value};
  REQUIRE(widened_value.has_value());
  REQUIRE(widened_value.value() == 3);

  auto move_only = result<move_only_box>::success(move_only_box{7});
  REQUIRE(move_only.has_value());
  REQUIRE(*move_only.value().value == 7);

  auto moved_out =
      std::move(move_only).value_or(move_only_box{9});
  REQUIRE(*moved_out.value == 7);

  const result<std::string> success_text{"safe"};
  REQUIRE(success_text.error() == wh::core::error_code{});

  auto failed = result<std::string>{errc::canceled};
  auto moved_error = std::move(failed).error();
  REQUIRE(moved_error == errc::canceled);
}

TEST_CASE("result reference specialization keeps aliasing semantics",
          "[UT][wh/core/result.hpp][result<T&>][branch][boundary]") {
  int source = 17;
  int fallback = 9;
  result<int &, errc> ref_result{source};

  REQUIRE(ref_result.has_value());
  REQUIRE(&ref_result.value() == &source);
  REQUIRE(ref_result.operator->() == &source);

  *ref_result = 21;
  REQUIRE(source == 21);
  REQUIRE(&ref_result.assume_value() == &source);
  REQUIRE(&ref_result.value_or(fallback) == &source);

  result<int &, errc> ref_failure{errc::queue_empty};
  REQUIRE(ref_failure.has_error());
  REQUIRE(ref_failure.error() == errc::queue_empty);
  REQUIRE(ref_failure.value_or(fallback) == fallback);
  REQUIRE(ref_failure.assume_error() == errc::queue_empty);

  long wide_source = 33;
  const result<long &, errc> wide_ref{wide_source};
  result<const long &, errc> converted{wide_ref};
  REQUIRE(converted.has_value());
  REQUIRE(&converted.value() == &wide_source);
}

TEST_CASE("result void specialization preserves success and failure semantics",
          "[UT][wh/core/result.hpp][result<void>][branch][boundary]") {
  const auto ok = result<void>::success();
  const auto ok_from_tag = result<void>{success()};
  const auto failed = result<void>{failure(errc::canceled)};
  const result<void> implicit_error = errc::queue_full;

  REQUIRE(ok.has_value());
  REQUIRE(ok_from_tag.has_value());
  REQUIRE_FALSE(failed.has_value());
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == errc::canceled);
  REQUIRE(failed.assume_error() == errc::canceled);
  REQUIRE(implicit_error.has_error());
  REQUIRE(implicit_error.error() == errc::queue_full);

  ok.value();
  REQUIRE(implicit_error.operator->() == nullptr);

  auto transient = result<void>{errc::timeout};
  transient.emplace();
  REQUIRE(transient.has_value());

  std::ostringstream stream{};
  stream << ok;
  REQUIRE(stream.str() == "value:void");
}

TEST_CASE("result pipe operators recover with value factory and result factory",
          "[UT][wh/core/result.hpp][operator|][branch][boundary]") {
  const result<int> ok_value{4};
  const result<int> err_value{errc::timeout};

  REQUIRE((ok_value | 11) == 4);
  REQUIRE((err_value | 11) == 11);
  REQUIRE((ok_value | [] { return 13; }) == 4);
  REQUIRE((err_value | [] { return 13; }) == 13);

  const auto ok_result = ok_value | [] { return result<long>{99}; };
  REQUIRE(ok_result.has_value());
  REQUIRE(ok_result.value() == 4);

  const auto recovered = err_value | [] { return result<int>{42}; };
  REQUIRE(recovered.has_value());
  REQUIRE(recovered.value() == 42);

  const result<void> ok_void{};
  const result<void> err_void{errc::queue_empty};
  REQUIRE((ok_void | [] { return result<void>{errc::timeout}; }).has_value());
  REQUIRE((err_void | [] { return result<void>{}; }).has_value());
}

TEST_CASE("result pipe assign operators mutate only error state",
          "[UT][wh/core/result.hpp][operator|=][branch]") {
  result<int> recover{errc::timeout};
  recover |= 12;
  REQUIRE(recover.has_value());
  REQUIRE(recover.value() == 12);

  result<int> recover_with_factory{errc::canceled};
  recover_with_factory |= [] { return 23; };
  REQUIRE(recover_with_factory.has_value());
  REQUIRE(recover_with_factory.value() == 23);

  result<int> recover_with_result{errc::queue_empty};
  recover_with_result |= [] { return result<int>{31}; };
  REQUIRE(recover_with_result.has_value());
  REQUIRE(recover_with_result.value() == 31);

  result<int> already_ok{8};
  already_ok |= [] { return 99; };
  REQUIRE(already_ok.value() == 8);
}

TEST_CASE("result and operators map values while preserving errors",
          "[UT][wh/core/result.hpp][operator&][branch]") {
  const result<int> ok_value{5};
  const result<int> err_value{errc::canceled};

  const auto mapped = ok_value & [](int value) { return value * 2; };
  REQUIRE(mapped.has_value());
  REQUIRE(mapped.value() == 10);

  const auto mapped_error = err_value & [](int value) { return value * 2; };
  REQUIRE(mapped_error.has_error());
  REQUIRE(mapped_error.error() == errc::canceled);

  const auto as_result = ok_value & [](int value) {
    return result<std::string>{std::to_string(value)};
  };
  REQUIRE(as_result.has_value());
  REQUIRE(as_result.value() == "5");

  const result<void> ok_void{};
  REQUIRE((ok_void & [] { return 7; }).value() == 7);
  REQUIRE((ok_void & [] { return result<void>{}; }).has_value());
}

TEST_CASE("result and assign operators update in place only for success state",
          "[UT][wh/core/result.hpp][operator&=][branch]") {
  result<int> map_ok{3};
  map_ok &= [](int value) { return value + 4; };
  REQUIRE(map_ok.has_value());
  REQUIRE(map_ok.value() == 7);

  result<int> map_ok_result{8};
  map_ok_result &= [](int value) { return result<int>{value * 2}; };
  REQUIRE(map_ok_result.has_value());
  REQUIRE(map_ok_result.value() == 16);

  result<int> map_error{errc::timeout};
  map_error &= [](int value) { return value + 100; };
  REQUIRE(map_error.has_error());
  REQUIRE(map_error.error() == errc::timeout);

  result<void> void_ok{};
  bool called = false;
  void_ok &= [&called] { called = true; };
  REQUIRE(called);

  result<void> void_ok_result{};
  void_ok_result &= [] { return result<void>{}; };
  REQUIRE(void_ok_result.has_value());
}

TEST_CASE("result static helpers and bool conversion distinguish success and failure",
          "[UT][wh/core/result.hpp][result::success][condition][boundary]") {
  const auto ok = result<std::string>::success("ready");
  const auto failed = result<std::string>::failure(errc::not_found);

  REQUIRE(static_cast<bool>(ok));
  REQUIRE_FALSE(static_cast<bool>(failed));
  REQUIRE(ok.value_or(std::string{"fallback"}) == "ready");
  REQUIRE(std::move(result<std::string>{errc::timeout})
              .value_or(std::string{"fallback"}) == "fallback");

  const auto void_ok = result<void>::success();
  const auto void_failed = result<void>::failure(errc::canceled);
  REQUIRE(void_ok.has_value());
  REQUIRE(void_failed.has_error());
  REQUIRE(void_failed.error() == errc::canceled);
}
