#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <type_traits>
#include <utility>

#include "wh/core/function/detail/acceptance_policy.hpp"
#include "wh/core/function/detail/error_policy.hpp"
#include "wh/core/function/detail/ownership_policy.hpp"
#include "wh/core/function/detail/storage_policy.hpp"

namespace {

struct small_functor {
  [[nodiscard]] auto operator()(const int value) const noexcept -> int {
    return value + 1;
  }
};

struct large_functor {
  std::array<int, 32> padding{};

  [[nodiscard]] auto operator()(const int value) const noexcept -> int {
    return value + 2;
  }
};

struct mutable_functor {
  int delta{0};

  [[nodiscard]] auto operator()(const int value) const noexcept -> int {
    return value + delta;
  }
};

struct move_only_functor {
  move_only_functor() = default;
  move_only_functor(const move_only_functor &) = delete;
  auto operator=(const move_only_functor &) -> move_only_functor & = delete;
  move_only_functor(move_only_functor &&) noexcept = default;
  auto operator=(move_only_functor &&) noexcept -> move_only_functor & = default;

  [[nodiscard]] auto operator()(const int value) const noexcept -> int {
    return value + 3;
  }
};

class owning_probe : public wh::core::fn::owning_storage<
                         int(int), wh::core::fn::reference_counting,
                         wh::core::fn::standard_accept,
                         wh::core::fn::skip_on_error, sizeof(void *),
                         std::allocator> {
  using base = wh::core::fn::owning_storage<
      int(int), wh::core::fn::reference_counting,
      wh::core::fn::standard_accept, wh::core::fn::skip_on_error,
      sizeof(void *), std::allocator>;

public:
  using typename base::buffer_type;

  template <typename fun_t>
  using traits_t = typename base::template traits<fun_t>;

  template <typename fun_t>
  static constexpr bool invocable_v = base::template is_invocable_v<fun_t>;

  template <typename fun_t, typename... args_t>
  static auto emplace(buffer_type &buffer, args_t &&...args) -> void {
    base::template create<fun_t>(buffer, std::forward<args_t>(args)...);
  }

  [[nodiscard]] static auto empty(const buffer_type &buffer) noexcept -> bool {
    return base::is_empty(buffer);
  }

  static auto clear(buffer_type &buffer) noexcept -> void {
    base::set_to_empty(buffer);
  }

  static auto clone(const buffer_type &source, buffer_type &destination) -> void {
    base::copy(source, destination);
  }

  static auto destroy_value(const buffer_type &buffer) noexcept -> void {
    base::destroy(buffer);
  }

  [[nodiscard]] static auto invoke(buffer_type &buffer, const int value) -> int {
    return base::access(buffer)(value);
  }
};

class ref_probe : public wh::core::fn::ref_only_storage<
                      int(int), wh::core::fn_detail::local_ownership,
                      wh::core::fn::ptr_accept, wh::core::fn::skip_on_error,
                      sizeof(void *), std::allocator> {
  using base = wh::core::fn::ref_only_storage<
      int(int), wh::core::fn_detail::local_ownership, wh::core::fn::ptr_accept,
      wh::core::fn::skip_on_error, sizeof(void *), std::allocator>;

public:
  using typename base::buffer_type;

  template <typename fun_t>
  using traits_t = typename base::template traits<fun_t>;

  template <typename fun_t>
  static constexpr bool invocable_v = base::template is_invocable_v<fun_t>;

  template <typename fun_t>
  static auto emplace(buffer_type &buffer, fun_t &&fun) -> void {
    base::template create<fun_t>(buffer, std::forward<fun_t>(fun));
  }

  [[nodiscard]] static auto empty(const buffer_type &buffer) noexcept -> bool {
    return base::is_empty(buffer);
  }

  static auto clear(buffer_type &buffer) noexcept -> void {
    base::set_to_empty(buffer);
  }

  static auto clone(const buffer_type &source, buffer_type &destination) -> void {
    base::copy(source, destination);
  }

  static auto destroy_value(const buffer_type &buffer) noexcept -> void {
    base::destroy(buffer);
  }

  [[nodiscard]] static auto invoke(buffer_type &buffer, const int value) -> int {
    return base::access(buffer)(value);
  }
};

static_assert(owning_probe::traits_t<small_functor>::using_soo);
static_assert(!owning_probe::traits_t<large_functor>::using_soo);
static_assert(!owning_probe::traits_t<small_functor>::storing_reference);
static_assert(ref_probe::traits_t<mutable_functor &>::using_soo);
static_assert(ref_probe::traits_t<mutable_functor &>::storing_reference);
static_assert(
    owning_probe::traits_t<small_functor>::template is_constructible<
        small_functor>);
static_assert(
    owning_probe::traits_t<small_functor>::template is_nothrow_constructible<
        small_functor>);
static_assert(owning_probe::invocable_v<small_functor>);
static_assert(!owning_probe::invocable_v<int>);
static_assert(ref_probe::invocable_v<mutable_functor *>);
static_assert(!ref_probe::invocable_v<move_only_functor>);
static_assert(wh::core::fn::is_same_storage_policy_v<
              wh::core::fn::owning_storage, wh::core::fn::owning_storage>);
static_assert(!wh::core::fn::is_same_storage_policy_v<
              wh::core::fn::owning_storage, wh::core::fn::ref_only_storage>);

} // namespace

TEST_CASE("storage policy owning and reference layouts construct clone destroy and classify targets",
          "[UT][wh/core/function/detail/storage_policy.hpp][owning_storage::create][condition][branch][boundary]") {
  owning_probe::buffer_type owning{};
  REQUIRE(owning_probe::empty(owning));
  owning_probe::clear(owning);
  REQUIRE(owning_probe::empty(owning));

  owning_probe::emplace<small_functor>(owning, small_functor{});
  REQUIRE_FALSE(owning_probe::empty(owning));
  REQUIRE(owning_probe::invoke(owning, 4) == 5);

  owning_probe::buffer_type cloned{};
  owning_probe::clone(owning, cloned);
  REQUIRE_FALSE(owning_probe::empty(cloned));
  REQUIRE(owning_probe::invoke(cloned, 9) == 10);

  owning_probe::destroy_value(owning);
  owning_probe::destroy_value(cloned);
  owning_probe::clear(owning);
  owning_probe::clear(cloned);
  REQUIRE(owning_probe::empty(owning));
  REQUIRE(owning_probe::empty(cloned));
  owning_probe::destroy_value(owning);
  owning_probe::destroy_value(cloned);
}

TEST_CASE("storage policy owning layout covers empty copy branch and heap-backed handlers",
          "[UT][wh/core/function/detail/storage_policy.hpp][owning_storage::copy][branch][boundary]") {
  owning_probe::buffer_type empty_source{};
  owning_probe::buffer_type empty_destination{};
  owning_probe::clone(empty_source, empty_destination);
  REQUIRE(owning_probe::empty(empty_destination));

  owning_probe::buffer_type heap_backed{};
  owning_probe::emplace<large_functor>(heap_backed, large_functor{});
  REQUIRE(owning_probe::invoke(heap_backed, 1) == 3);
  owning_probe::buffer_type heap_clone{};
  owning_probe::clone(heap_backed, heap_clone);
  REQUIRE_FALSE(owning_probe::empty(heap_clone));
  REQUIRE(owning_probe::invoke(heap_clone, 5) == 7);
  owning_probe::destroy_value(heap_backed);
  owning_probe::destroy_value(heap_clone);
  owning_probe::clear(heap_backed);
  owning_probe::clear(heap_clone);
  REQUIRE(owning_probe::empty(heap_backed));
  REQUIRE(owning_probe::empty(heap_clone));
}

TEST_CASE("storage policy reference layout preserves aliasing across source and clone",
          "[UT][wh/core/function/detail/storage_policy.hpp][ref_only_storage::create][condition][branch]") {
  mutable_functor functor{.delta = 4};
  ref_probe::buffer_type referenced{};
  REQUIRE(ref_probe::empty(referenced));
  ref_probe::emplace(referenced, functor);
  REQUIRE_FALSE(ref_probe::empty(referenced));
  REQUIRE(ref_probe::invoke(referenced, 2) == 6);

  functor.delta = 8;
  REQUIRE(ref_probe::invoke(referenced, 2) == 10);

  ref_probe::buffer_type referenced_copy{};
  ref_probe::clone(referenced, referenced_copy);
  REQUIRE(ref_probe::invoke(referenced_copy, 1) == 9);
  functor.delta = 1;
  REQUIRE(ref_probe::invoke(referenced, 2) == 3);
  REQUIRE(ref_probe::invoke(referenced_copy, 2) == 3);
  ref_probe::destroy_value(referenced);
  ref_probe::destroy_value(referenced_copy);
  ref_probe::clear(referenced);
  ref_probe::clear(referenced_copy);
  REQUIRE(ref_probe::empty(referenced));
  REQUIRE(ref_probe::empty(referenced_copy));
  ref_probe::destroy_value(referenced);
  ref_probe::destroy_value(referenced_copy);
}

TEST_CASE("storage policy traits expose invocability and template identity",
          "[UT][wh/core/function/detail/storage_policy.hpp][is_same_storage_policy_v][branch]") {
  REQUIRE(owning_probe::traits_t<small_functor>::using_soo);
  REQUIRE_FALSE(owning_probe::traits_t<large_functor>::using_soo);
  REQUIRE_FALSE(owning_probe::traits_t<small_functor>::storing_reference);
  REQUIRE(ref_probe::traits_t<mutable_functor &>::using_soo);
  REQUIRE(ref_probe::traits_t<mutable_functor &>::storing_reference);
  REQUIRE(owning_probe::invocable_v<small_functor>);
  REQUIRE_FALSE(owning_probe::invocable_v<int>);
  REQUIRE(ref_probe::invocable_v<mutable_functor *>);
  REQUIRE_FALSE(ref_probe::invocable_v<move_only_functor>);
  REQUIRE(wh::core::fn::is_same_storage_policy_v<
          wh::core::fn::owning_storage, wh::core::fn::owning_storage>);
  REQUIRE_FALSE(wh::core::fn::is_same_storage_policy_v<
                wh::core::fn::owning_storage, wh::core::fn::ref_only_storage>);
}
