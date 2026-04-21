#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/function/detail/vtable.hpp"

namespace {

struct adder {
  int base{0};

  [[nodiscard]] auto operator()(int value) const -> int { return base + value; }
};

struct move_only_adder {
  std::unique_ptr<int> base{};

  explicit move_only_adder(int next) : base(std::make_unique<int>(next)) {}
  move_only_adder(const move_only_adder &) = delete;
  auto operator=(const move_only_adder &) -> move_only_adder & = delete;
  move_only_adder(move_only_adder &&) noexcept = default;
  auto operator=(move_only_adder &&) noexcept -> move_only_adder & = default;

  [[nodiscard]] auto operator()(int value) const -> int { return *base + value; }
};

using clone_handler = wh::core::fn_detail::vtable_handler<
    int(int), adder, wh::core::fn_detail::cloneable_vtable<int(int)>, wh::core::fn::deep_copy,
    sizeof(void *), false, std::allocator>;
using move_handler = wh::core::fn_detail::vtable_handler<
    int(int), move_only_adder, wh::core::fn_detail::destructible_vtable<int(int)>,
    wh::core::fn::reference_counting, sizeof(void *), false, std::allocator>;
using pointer_handler = wh::core::fn_detail::vtable_handler<
    int(int), std::unique_ptr<adder>, wh::core::fn_detail::destructible_vtable<int(int)>,
    wh::core::fn::reference_counting, sizeof(void *), true, std::allocator>;

static_assert(std::has_virtual_destructor_v<wh::core::fn_detail::cloneable_vtable<int(int)>>);
static_assert(std::has_virtual_destructor_v<wh::core::fn_detail::destructible_vtable<int(int)>>);

} // namespace

TEST_CASE("vtable base types expose the expected virtual destruction contract",
          "[UT][wh/core/function/detail/vtable.hpp][cloneable_vtable][branch]") {
  REQUIRE(std::has_virtual_destructor_v<wh::core::fn_detail::cloneable_vtable<int(int)>>);
  REQUIRE(std::has_virtual_destructor_v<wh::core::fn_detail::destructible_vtable<int(int)>>);
}

TEST_CASE("cloneable vtable handlers invoke and clone copyable call targets",
          "[UT][wh/core/function/detail/vtable.hpp][vtable_handler][condition][branch]") {
  clone_handler handler{adder{2}};
  REQUIRE(handler(3) == 5);
  REQUIRE(clone_handler::using_soo);

  wh::core::fn_detail::any_storage<sizeof(clone_handler)> storage{};
  handler.clone_itself(storage.address());
  auto &cloned = storage.interpret_as<clone_handler &>();
  REQUIRE(cloned(4) == 6);
  std::destroy_at(&cloned);
}

TEST_CASE("destructible vtable handlers support move-only and pointer-backed targets",
          "[UT][wh/core/function/detail/vtable.hpp][destructible_vtable][branch][boundary]") {
  move_handler move_only{move_only_adder{5}};
  REQUIRE(move_only(4) == 9);

  pointer_handler pointer_backed{std::make_unique<adder>(adder{7})};
  REQUIRE(pointer_backed(2) == 9);
}
