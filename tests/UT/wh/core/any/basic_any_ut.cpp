#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "wh/core/any/basic_any.hpp"

namespace {

using basic_any_t = wh::core::basic_any<>;

} // namespace

TEST_CASE("basic_any stores metadata and typed payload access",
          "[UT][wh/core/any/basic_any.hpp][basic_any][branch]") {
  basic_any_t value{std::string{"seed"}};

  REQUIRE(value.has_value());
  REQUIRE(value.has_value<std::string>());
  REQUIRE(value.owner());
  REQUIRE_FALSE(value.borrowed());
  REQUIRE(value.copyable());
  REQUIRE((value.policy() == wh::core::any_policy::inline_owner ||
           value.policy() == wh::core::any_policy::heap_owner));
  REQUIRE(value.key() == basic_any_t::type_key<std::string>());
  REQUIRE(value.info() == basic_any_t::info_of<std::string>());
  REQUIRE(value.type() == typeid(std::string));

  auto *typed = value.data<std::string>();
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == "seed");
  REQUIRE(value.get_if<std::string>() == typed);
}

TEST_CASE("basic_any copy move reset and move-only ownership semantics",
          "[UT][wh/core/any/basic_any.hpp][basic_any::reset][branch][boundary]") {
  basic_any_t owned{std::string{"alpha"}};
  basic_any_t copied{owned};
  basic_any_t moved{std::move(owned)};

  REQUIRE(copied.has_value<std::string>());
  REQUIRE(*copied.data<std::string>() == "alpha");
  REQUIRE(moved.has_value<std::string>());
  REQUIRE(*moved.data<std::string>() == "alpha");
  REQUIRE_FALSE(owned.has_value());

  moved.reset();
  REQUIRE_FALSE(moved.has_value());
  REQUIRE(moved.policy() == wh::core::any_policy::empty);

  basic_any_t move_only{std::in_place_type<std::unique_ptr<int>>};
  *move_only.data<std::unique_ptr<int>>() = std::make_unique<int>(9);
  basic_any_t move_only_copy{move_only};
  basic_any_t move_only_move{std::move(move_only)};

  REQUIRE_FALSE(move_only_copy.has_value());
  REQUIRE_FALSE(move_only_copy.copyable());
  REQUIRE(move_only_move.has_value<std::unique_ptr<int>>());
  REQUIRE(**move_only_move.data<std::unique_ptr<int>>() == 9);
}

TEST_CASE("basic_any borrowed views and assign preserve aliasing rules",
          "[UT][wh/core/any/basic_any.hpp][basic_any::assign][branch][boundary]") {
  int value = 7;
  const int const_value = 11;

  auto ref = basic_any_t::ref(value);
  auto cref = basic_any_t::cref(const_value);
  auto ref_copy = ref;
  auto as_ref = ref.as_ref();

  REQUIRE(ref.borrowed());
  REQUIRE(cref.borrowed());
  REQUIRE(ref.policy() == wh::core::any_policy::ref);
  REQUIRE(cref.policy() == wh::core::any_policy::cref);
  REQUIRE(ref.data<int>() == &value);
  REQUIRE(std::as_const(cref).data<const int>() == &const_value);
  REQUIRE(ref_copy.data<int>() == &value);
  REQUIRE(as_ref.data<int>() == &value);

  REQUIRE(ref.assign(9));
  REQUIRE(value == 9);
  REQUIRE_FALSE(cref.assign(13));
  REQUIRE(const_value == 11);
}

TEST_CASE("basic_any equality and mismatched data lookups are type-safe",
          "[UT][wh/core/any/basic_any.hpp][operator==][branch][boundary]") {
  basic_any_t first{3};
  basic_any_t second{3};
  basic_any_t different{4};
  basic_any_t empty{};

  REQUIRE(first == second);
  REQUIRE_FALSE(first == different);
  REQUIRE(empty == basic_any_t{});
  REQUIRE(first.data<double>() == nullptr);
  REQUIRE(first.data(wh::core::any_type_key{}) == nullptr);
}

TEST_CASE("basic_any handles empty aliases key queries and compatible assignment branches",
          "[UT][wh/core/any/basic_any.hpp][basic_any::assign][condition][boundary]") {
  basic_any_t empty{};
  REQUIRE_FALSE(empty.as_ref().has_value());
  REQUIRE(empty.data() == nullptr);

  basic_any_t target{std::string{"seed"}};
  REQUIRE(target.has_value(target.key()));
  REQUIRE(target.has_value(target.info()));

  basic_any_t compatible{std::string{"left"}};
  REQUIRE(target.assign(std::as_const(compatible)));
  REQUIRE(*target.data<std::string>() == "left");

  const std::string constant = "right";
  auto cref = basic_any_t::cref(constant);
  REQUIRE(target.assign(std::as_const(cref)));
  REQUIRE(*target.data<std::string>() == "right");

  basic_any_t moved_source{std::string{"moved"}};
  REQUIRE(target.assign(std::move(moved_source)));
  REQUIRE(*target.data<std::string>() == "moved");

  REQUIRE_FALSE(target.assign(basic_any_t{7}));
  REQUIRE_FALSE(cref.assign(std::string{"fail"}));
  REQUIRE(cref.borrowed());
  REQUIRE(cref.data<std::string>() == nullptr);
  REQUIRE(std::as_const(cref).data<std::string>() == &constant);
}
