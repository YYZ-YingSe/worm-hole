#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/type_traits.hpp"

namespace {

struct not_container {};

struct callable_probe {
  auto operator()(const int value) const noexcept -> long { return static_cast<long>(value + 1); }
};

} // namespace

static_assert(std::same_as<wh::core::remove_cvref_t<const int &>, int>);
static_assert(std::same_as<wh::core::remove_cvref_t<volatile int &&>, int>);

static_assert(wh::core::container_like<std::vector<int>>);
static_assert(!wh::core::container_like<int>);
static_assert(!wh::core::container_like<not_container>);

static_assert(wh::core::pair_like<std::pair<int, int>>);
static_assert(!wh::core::pair_like<std::vector<int>>);

static_assert(wh::core::is_optional_v<std::optional<int>>);
static_assert(!wh::core::is_optional_v<int>);

static_assert(wh::core::is_unique_ptr_v<std::unique_ptr<int>>);
static_assert(!wh::core::is_unique_ptr_v<int *>);

static_assert(wh::core::is_shared_ptr_v<std::shared_ptr<int>>);
static_assert(!wh::core::is_shared_ptr_v<int *>);

static_assert(wh::core::is_raw_pointer_v<int *>);
static_assert(!wh::core::is_raw_pointer_v<std::unique_ptr<int>>);

static_assert(wh::core::is_pointer_like_v<int *>);
static_assert(wh::core::is_pointer_like_v<std::unique_ptr<int>>);
static_assert(wh::core::is_pointer_like_v<std::shared_ptr<int>>);
static_assert(!wh::core::is_pointer_like_v<int>);

static_assert(wh::core::callable_with<callable_probe, int>);
static_assert(!wh::core::callable_with<callable_probe, std::string>);
static_assert(std::same_as<wh::core::callable_result_t<callable_probe, int>, long>);

static_assert(wh::core::is_result_v<wh::core::result<int>>);
static_assert(wh::core::result_like<wh::core::result<int>>);
static_assert(!wh::core::is_result_v<int>);
static_assert(!wh::core::result_like<int>);

TEST_CASE("transparent_string_hash supports heterogeneous string keys",
          "[UT][wh/core/type_traits.hpp][transparent_string_hash][condition][branch][boundary]") {
  const wh::core::transparent_string_hash hash{};
  const std::string owned = "worm-hole";
  const std::string_view viewed = owned;
  const char *cstr = "worm-hole";

  REQUIRE(hash(owned) == hash(viewed));
  REQUIRE(hash(viewed) == hash(cstr));

  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      values{};
  values.insert(owned);

  REQUIRE(values.contains(viewed));
  REQUIRE(values.contains(cstr));
}

TEST_CASE("transparent_string_equal compares heterogeneous string views",
          "[UT][wh/core/type_traits.hpp][transparent_string_equal][condition][branch]") {
  const wh::core::transparent_string_equal equal{};

  REQUIRE(equal("alpha", std::string_view{"alpha"}));
  REQUIRE(equal(std::string_view{"beta"}, std::string_view{"beta"}));
  REQUIRE_FALSE(equal("alpha", "gamma"));
}

TEST_CASE("type_traits concepts classify containers pairs optionals pointers callables and results",
          "[UT][wh/core/type_traits.hpp][container_like][condition][branch][boundary]") {
  STATIC_REQUIRE(std::same_as<wh::core::remove_cvref_t<const int &>, int>);
  STATIC_REQUIRE(std::same_as<wh::core::remove_cvref_t<volatile int &&>, int>);

  STATIC_REQUIRE(wh::core::container_like<std::vector<int>>);
  STATIC_REQUIRE_FALSE(wh::core::container_like<int>);
  STATIC_REQUIRE_FALSE(wh::core::container_like<not_container>);

  STATIC_REQUIRE(wh::core::pair_like<std::pair<int, int>>);
  STATIC_REQUIRE_FALSE(wh::core::pair_like<std::vector<int>>);

  STATIC_REQUIRE(wh::core::is_optional_v<std::optional<int>>);
  STATIC_REQUIRE_FALSE(wh::core::is_optional_v<int>);

  STATIC_REQUIRE(wh::core::is_unique_ptr_v<std::unique_ptr<int>>);
  STATIC_REQUIRE_FALSE(wh::core::is_unique_ptr_v<int *>);
  STATIC_REQUIRE(wh::core::is_shared_ptr_v<std::shared_ptr<int>>);
  STATIC_REQUIRE_FALSE(wh::core::is_shared_ptr_v<int *>);
  STATIC_REQUIRE(wh::core::is_raw_pointer_v<int *>);
  STATIC_REQUIRE_FALSE(wh::core::is_raw_pointer_v<std::unique_ptr<int>>);
  STATIC_REQUIRE(wh::core::is_pointer_like_v<int *>);
  STATIC_REQUIRE(wh::core::is_pointer_like_v<std::unique_ptr<int>>);
  STATIC_REQUIRE(wh::core::is_pointer_like_v<std::shared_ptr<int>>);
  STATIC_REQUIRE_FALSE(wh::core::is_pointer_like_v<int>);

  STATIC_REQUIRE(wh::core::callable_with<callable_probe, int>);
  STATIC_REQUIRE_FALSE(wh::core::callable_with<callable_probe, std::string>);
  STATIC_REQUIRE(std::same_as<wh::core::callable_result_t<callable_probe, int>, long>);

  STATIC_REQUIRE(wh::core::is_result_v<wh::core::result<int>>);
  STATIC_REQUIRE(wh::core::result_like<wh::core::result<int>>);
  STATIC_REQUIRE_FALSE(wh::core::is_result_v<int>);
  STATIC_REQUIRE_FALSE(wh::core::result_like<int>);
}
