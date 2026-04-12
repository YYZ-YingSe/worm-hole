#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "wh/core/small_vector/concepts.hpp"

static_assert(wh::core::small_vector_like<wh::core::small_vector<int>>);
static_assert(!wh::core::small_vector_like<std::vector<int>>);

TEST_CASE("small_vector concepts accept project small_vector types",
          "[UT][wh/core/small_vector/concepts.hpp][small_vector_like][condition][branch][boundary]") {
  wh::core::small_vector<int> values{};
  values.push_back(1);
  REQUIRE(values.size() == 1U);
}

TEST_CASE("small_vector_like rejects std::vector and preserves std round-trip surface on project vectors",
          "[UT][wh/core/small_vector/concepts.hpp][small_vector_like][condition][branch]") {
  STATIC_REQUIRE(wh::core::small_vector_like<wh::core::small_vector<int>>);
  STATIC_REQUIRE_FALSE(wh::core::small_vector_like<std::vector<int>>);

  wh::core::small_vector<int> values{};
  values.push_back(2);
  const auto inserted = values.insert(values.begin(), 1);
  REQUIRE(*inserted == 1);
  const auto erased = values.erase(values.begin());
  REQUIRE(*erased == 2);

  const auto standard = values.to_std_vector();
  REQUIRE(standard.size() == 1U);
  REQUIRE(standard.front() == 2);

  auto reconstructed = wh::core::small_vector<int>::from_std_vector(standard);
  REQUIRE(reconstructed.size() == 1U);
  REQUIRE(reconstructed.front() == 2);
}
