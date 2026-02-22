#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <type_traits>

#include "wh/core/type_utils.hpp"

namespace {

struct sample_pair {
  using first_type = int;
  using second_type = double;

  int first{};
  double second{};
};

struct sample_functor {
  auto operator()(int lhs, double rhs) const -> long {
    return static_cast<long>(lhs + rhs);
  }
};

} // namespace

TEST_CASE("type_utils metaprogramming traits contract",
          "[core][type_utils][condition]") {
  static_assert(std::same_as<wh::core::type_of_t<const int &>, int>);
  static_assert(std::same_as<decltype(wh::core::type_of<const int &>()),
                             wh::core::type_tag<int>>);

  static_assert(wh::core::container_like<std::vector<int>>);
  static_assert(wh::core::pair_like<sample_pair>);
  static_assert(wh::core::callable_with<sample_functor, int, double>);
  static_assert(
      std::same_as<wh::core::callable_result_t<sample_functor, int, double>,
                   long>);

  using args_t = wh::core::function_argument_types_t<sample_functor>;
  static_assert(wh::core::type_list_size<args_t>::value == 2);
  static_assert(
      std::same_as<typename wh::core::type_list_at<0, args_t>::type, int>);
  static_assert(
      std::same_as<typename wh::core::type_list_at<1, args_t>::type, double>);

  using reversed_t = typename wh::core::type_list_reverse<args_t>::type;
  static_assert(
      std::same_as<typename wh::core::type_list_at<0, reversed_t>::type,
                   double>);
  static_assert(
      std::same_as<typename wh::core::type_list_at<1, reversed_t>::type, int>);

  SUCCEED();
}

TEST_CASE("type_utils object helpers branch behavior",
          "[core][type_utils][branch]") {
  auto owned = wh::core::wrap_unique(std::tuple<int, int>{1, 2});
  REQUIRE(std::get<0>(*owned) == 1);
  REQUIRE(std::get<1>(*owned) == 2);

  using return_t = wh::core::function_return_t<sample_functor>;
  REQUIRE((std::is_same_v<return_t, long>));
}

TEST_CASE("type_utils default instance for container edge path",
          "[core][type_utils][extreme]") {
  const auto values = wh::core::default_instance<std::vector<int>>();
  REQUIRE(values.empty());
}
