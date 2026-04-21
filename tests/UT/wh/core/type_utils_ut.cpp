#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/type_utils.hpp"

namespace ut_support {

struct widget {
  int value{7};

  auto operator==(const widget &) const -> bool = default;
};

struct no_default {
  no_default() = delete;
};

using sample_types = wh::core::type_list<int, double, char>;
using reversed_sample_types = wh::core::type_list<char, double, int>;

} // namespace ut_support

namespace wh::internal {

template <> struct type_alias<ut_support::widget> {
  static constexpr std::string_view value = "ut.widget";
};

} // namespace wh::internal

static_assert(std::same_as<wh::core::type_of_t<const int &>, int>);
static_assert(std::same_as<decltype(wh::core::type_of<const long &>()), wh::core::type_tag<long>>);

static_assert(wh::core::default_initializable_object<ut_support::widget>);
static_assert(!wh::core::default_initializable_object<ut_support::no_default>);

static_assert(wh::core::type_list_size<ut_support::sample_types>::value == 3U);
static_assert(std::same_as<wh::core::type_list_at<0U, ut_support::sample_types>::type, int>);
static_assert(std::same_as<wh::core::type_list_at<1U, ut_support::sample_types>::type, double>);
static_assert(std::same_as<wh::core::type_list_at<2U, ut_support::sample_types>::type, char>);
static_assert(std::same_as<wh::core::type_list_reverse<ut_support::sample_types>::type,
                           ut_support::reversed_sample_types>);

TEST_CASE("has_instance distinguishes empty and engaged pointer-like holders",
          "[UT][wh/core/type_utils.hpp][has_instance][branch][boundary]") {
  int value = 11;
  int *raw = &value;
  int *null_raw = nullptr;
  std::unique_ptr<int> unique = std::make_unique<int>(13);
  std::unique_ptr<int> empty_unique{};
  std::shared_ptr<int> shared = std::make_shared<int>(17);
  std::shared_ptr<int> empty_shared{};
  ut_support::widget object{};

  REQUIRE(wh::core::has_instance(raw));
  REQUIRE_FALSE(wh::core::has_instance(null_raw));
  REQUIRE(wh::core::has_instance(unique));
  REQUIRE_FALSE(wh::core::has_instance(empty_unique));
  REQUIRE(wh::core::has_instance(shared));
  REQUIRE_FALSE(wh::core::has_instance(empty_shared));
  REQUIRE(wh::core::has_instance(object));
}

TEST_CASE("deref_or_self returns pointee for pointer-like values and self otherwise",
          "[UT][wh/core/type_utils.hpp][deref_or_self][branch]") {
  int raw_value = 5;
  int *raw = &raw_value;
  std::unique_ptr<int> unique = std::make_unique<int>(7);
  const ut_support::widget constant_widget{.value = 9};
  ut_support::widget object{.value = 3};

  wh::core::deref_or_self(raw) = 6;
  wh::core::deref_or_self(unique) = 8;
  wh::core::deref_or_self(object).value = 4;

  REQUIRE(raw_value == 6);
  REQUIRE(*unique == 8);
  REQUIRE(object.value == 4);
  REQUIRE(wh::core::deref_or_self(constant_widget).value == 9);
}

TEST_CASE("default_instance creates values and raw pointer pointees",
          "[UT][wh/core/type_utils.hpp][default_instance][boundary]") {
  auto value_result = wh::core::default_instance<ut_support::widget>();
  REQUIRE(value_result.has_value());
  REQUIRE(value_result.value() == ut_support::widget{});

  auto pointer_result = wh::core::default_instance<int *>();
  REQUIRE(pointer_result.has_value());
  REQUIRE(pointer_result.value() != nullptr);
  REQUIRE(*pointer_result.value() == 0);
  delete pointer_result.value();
}

TEST_CASE("wrap_unique reverse_copy and map_copy_as preserve values",
          "[UT][wh/core/type_utils.hpp][wrap_unique][boundary]") {
  auto wrapped = wh::core::wrap_unique(ut_support::widget{.value = 21});
  REQUIRE(wrapped.has_value());
  REQUIRE(wrapped.value() != nullptr);
  REQUIRE(wrapped.value()->value == 21);

  const std::vector<int> sequence{1, 2, 3, 4};
  auto reversed = wh::core::reverse_copy(sequence);
  REQUIRE(reversed.has_value());
  REQUIRE(reversed.value() == std::vector<int>({4, 3, 2, 1}));

  const std::vector<int> empty_sequence{};
  auto reversed_empty = wh::core::reverse_copy(empty_sequence);
  REQUIRE(reversed_empty.has_value());
  REQUIRE(reversed_empty.value().empty());

  const std::map<std::string, int> mapping{
      {"left", 1},
      {"right", 2},
  };
  auto copied = wh::core::map_copy_as<std::unordered_map<std::string, int>>(mapping);
  REQUIRE(copied.has_value());
  REQUIRE(copied.value().size() == mapping.size());
  REQUIRE(copied.value().at("left") == 1);
  REQUIRE(copied.value().at("right") == 2);
}

TEST_CASE("stable and diagnostic type tokens are non-empty and stable",
          "[UT][wh/core/type_utils.hpp][stable_type_token][boundary]") {
  const auto stable_first = wh::core::stable_type_token<ut_support::widget>();
  const auto stable_second = wh::core::stable_type_token<ut_support::widget>();
  const auto diagnostic_first = wh::core::diagnostic_type_token<ut_support::widget>();
  const auto diagnostic_second = wh::core::diagnostic_type_token<ut_support::widget>();

  REQUIRE_FALSE(stable_first.empty());
  REQUIRE_FALSE(diagnostic_first.empty());
  REQUIRE(stable_first == stable_second);
  REQUIRE(diagnostic_first == diagnostic_second);
  REQUIRE(stable_first == "ut.widget");
  REQUIRE(diagnostic_first == "ut.widget");
}

TEST_CASE("type utils expose default-initializable constraints and wrap_unique success",
          "[UT][wh/core/type_utils.hpp][default_initializable_object][condition][branch]") {
  STATIC_REQUIRE(wh::core::default_initializable_object<ut_support::widget>);
  STATIC_REQUIRE(!wh::core::default_initializable_object<ut_support::no_default>);

  auto wrapped = wh::core::wrap_unique(ut_support::widget{.value = 5});
  REQUIRE(wrapped.has_value());
  REQUIRE(wrapped.value() != nullptr);
  REQUIRE(wrapped.value()->value == 5);
}
