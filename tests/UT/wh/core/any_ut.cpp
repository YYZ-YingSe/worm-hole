#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/any.hpp"

namespace {

struct move_only_value {
  explicit move_only_value(int next) : payload(std::make_unique<int>(next)) {}

  move_only_value(move_only_value &&) noexcept = default;
  auto operator=(move_only_value &&) noexcept -> move_only_value & = default;

  move_only_value(const move_only_value &) = delete;
  auto operator=(const move_only_value &) -> move_only_value & = delete;

  std::unique_ptr<int> payload{};
};

using wh::core::any;
using wh::core::any_cast;
using wh::core::any_info_v;
using wh::core::any_policy;
using wh::core::any_type_key_v;
using wh::core::forward_as_any;
using wh::core::make_any;

static_assert(std::same_as<any, wh::core::basic_any<>>);

} // namespace

TEST_CASE("any type metadata aliases basic_any metadata",
          "[UT][wh/core/any.hpp][any_info_v][branch]") {
  REQUIRE(any_type_key_v<int> == any::type_key<int>());
  REQUIRE(any_type_key_v<int> != any_type_key_v<long>);
  REQUIRE(any_info_v<std::string> == any::info_of<std::string>());
  REQUIRE(any_info_v<std::string>.key == any_type_key_v<std::string>);
  REQUIRE(any_info_v<std::string>.size == sizeof(std::string));
  REQUIRE(any_info_v<std::string>.alignment == alignof(std::string));
  REQUIRE(any_info_v<std::string>.type() == typeid(std::string));
}

TEST_CASE("make_any constructs owned payloads in place",
          "[UT][wh/core/any.hpp][make_any][branch][boundary]") {
  auto value = make_any<std::string>(3U, 'x');

  REQUIRE(value.has_value<std::string>());
  REQUIRE(value.owner());
  REQUIRE((value.policy() == any_policy::inline_owner || value.policy() == any_policy::heap_owner));

  auto *typed = any_cast<std::string>(&value);
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == "xxx");
}

TEST_CASE("forward_as_any borrows lvalues and owns rvalues",
          "[UT][wh/core/any.hpp][forward_as_any][branch][boundary]") {
  std::string text = "seed";
  const int const_value = 11;

  auto borrowed = forward_as_any(text);
  auto owned = forward_as_any(std::string{"temp"});
  auto cref = forward_as_any(const_value);

  REQUIRE(borrowed.policy() == any_policy::ref);
  REQUIRE_FALSE(borrowed.owner());
  REQUIRE(any_cast<std::string>(&borrowed) == &text);

  REQUIRE(owned.owner());
  REQUIRE(any_cast<std::string>(std::as_const(owned)) == "temp");

  REQUIRE(cref.policy() == any_policy::cref);
  REQUIRE(any_cast<const int>(&std::as_const(cref)) == &const_value);
}

TEST_CASE("any_cast pointer overloads handle null const mismatch and success",
          "[UT][wh/core/any.hpp][any_cast<T*>][branch][boundary]") {
  any value{std::string{"payload"}};

  REQUIRE(any_cast<std::string>(static_cast<any *>(nullptr)) == nullptr);
  REQUIRE(any_cast<std::string>(static_cast<const any *>(nullptr)) == nullptr);
  REQUIRE(any_cast<int>(&value) == nullptr);
  REQUIRE(any_cast<const int>(&std::as_const(value)) == nullptr);

  auto *typed = any_cast<std::string>(&value);
  auto *const_typed = any_cast<const std::string>(&std::as_const(value));
  REQUIRE(typed != nullptr);
  REQUIRE(const_typed == typed);
  REQUIRE(*typed == "payload");
}

TEST_CASE("any_cast reference and value overloads preserve copy and alias semantics",
          "[UT][wh/core/any.hpp][any_cast<T>][branch]") {
  any value{std::string{"payload"}};

  auto copied = any_cast<std::string>(std::as_const(value));
  auto &ref = any_cast<std::string &>(value);

  REQUIRE(copied == "payload");
  REQUIRE(&ref == any_cast<std::string>(&value));

  ref = "updated";
  REQUIRE(any_cast<std::string>(std::as_const(value)) == "updated");

  const any const_value{std::string{"const"}};
  REQUIRE(any_cast<std::string>(std::move(const_value)) == "const");
}

TEST_CASE("any_cast rvalue overload moves move-only payloads out of owned any",
          "[UT][wh/core/any.hpp][any_cast<T&&>][boundary]") {
  any value{std::in_place_type<move_only_value>, 7};

  auto extracted = any_cast<move_only_value>(std::move(value));

  REQUIRE(extracted.payload != nullptr);
  REQUIRE(*extracted.payload == 7);
}

TEST_CASE("any_cast supports const-qualified queries and borrowed aliases",
          "[UT][wh/core/any.hpp][any_cast<const T>][branch]") {
  int number = 42;
  auto borrowed = forward_as_any(number);
  auto const_view = std::as_const(borrowed).as_ref();

  auto *mutable_ptr = any_cast<int>(&borrowed);
  auto *const_ptr = any_cast<const int>(&std::as_const(borrowed));
  auto copied = any_cast<int>(const_view);

  REQUIRE(mutable_ptr == &number);
  REQUIRE(const_ptr == &number);
  REQUIRE(copied == 42);
}

TEST_CASE("any facade preserves borrowed rvalue extraction and facade aliases",
          "[UT][wh/core/any.hpp][any_cast<any&&>][condition][boundary]") {
  int number = 9;
  auto borrowed = forward_as_any(number);
  REQUIRE(borrowed.key() == any_type_key_v<int>);
  REQUIRE(borrowed.info() == any_info_v<int>);

  auto copied = any_cast<int>(std::move(borrowed));
  REQUIRE(copied == 9);
  REQUIRE(number == 9);

  const any owned = make_any<std::string>(2U, 'a');
  auto moved_copy = any_cast<std::string>(std::move(owned));
  REQUIRE(moved_copy == "aa");
}
