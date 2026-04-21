#include <atomic>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/function/detail/ownership_policy.hpp"

namespace {

struct tracked_value {
  inline static std::atomic<int> copies{0};
  inline static std::atomic<int> destructions{0};

  static auto reset() -> void {
    copies.store(0);
    destructions.store(0);
  }

  tracked_value() = default;
  explicit tracked_value(int next) : value(next) {}
  tracked_value(const tracked_value &other) : value(other.value) { copies.fetch_add(1); }
  auto operator=(const tracked_value &) -> tracked_value & = default;
  ~tracked_value() { destructions.fetch_add(1); }

  int value{0};
};

template <typename target_t>
struct local_probe : wh::core::fn_detail::local_ownership<target_t, std::allocator> {
  using base = wh::core::fn_detail::local_ownership<target_t, std::allocator>;
  using typename base::allocator_type;
  using typename base::stored_type;

  template <typename... args_t>
  static auto create(allocator_type &alloc, stored_type *target, args_t &&...args) -> void {
    base::create(alloc, target, std::forward<args_t>(args)...);
  }

  static auto copy(allocator_type &alloc, const stored_type &source, stored_type *destination)
      -> void {
    base::copy(alloc, source, destination);
  }

  static auto destroy(allocator_type &alloc, stored_type *target) -> void {
    base::destroy(alloc, target);
  }

  [[nodiscard]] static auto get(const stored_type &stored) -> const target_t & {
    return base::get_target(stored);
  }
};

template <typename target_t> struct deep_probe : wh::core::fn::deep_copy<target_t, std::allocator> {
  using base = wh::core::fn::deep_copy<target_t, std::allocator>;
  using typename base::allocator_type;
  using typename base::stored_type;

  template <typename... args_t>
  static auto create(allocator_type &alloc, stored_type *target, args_t &&...args) -> void {
    base::create(alloc, target, std::forward<args_t>(args)...);
  }

  static auto copy(allocator_type &alloc, const stored_type &source, stored_type *destination)
      -> void {
    base::copy(alloc, source, destination);
  }

  static auto destroy(allocator_type &alloc, stored_type *target) -> void {
    base::destroy(alloc, target);
  }

  [[nodiscard]] static auto get(const stored_type &stored) -> const target_t & {
    return base::get_target(stored);
  }
};

template <typename target_t>
struct ref_probe : wh::core::fn::reference_counting<target_t, std::allocator> {
  using base = wh::core::fn::reference_counting<target_t, std::allocator>;
  using typename base::allocator_type;
  using typename base::stored_type;

  template <typename... args_t>
  static auto create(allocator_type &alloc, stored_type *target, args_t &&...args) -> void {
    base::create(alloc, target, std::forward<args_t>(args)...);
  }

  static auto copy(allocator_type &alloc, const stored_type &source, stored_type *destination)
      -> void {
    base::copy(alloc, source, destination);
  }

  static auto destroy(allocator_type &alloc, stored_type *target) -> void {
    base::destroy(alloc, target);
  }

  [[nodiscard]] static auto get(const stored_type &stored) -> const target_t & {
    return base::get_target(stored);
  }
};

} // namespace

TEST_CASE("reference_counting_wrapper keeps object alive until last destroy",
          "[UT][wh/core/function/detail/"
          "ownership_policy.hpp][reference_counting_wrapper][branch][concurrency]") {
  using wrapper = wh::core::fn_detail::reference_counting_wrapper<tracked_value, std::allocator>;

  tracked_value::reset();
  auto alloc = wrapper::allocator_type{};
  auto *first = wrapper::create_new(alloc, 9);
  auto *second = wrapper::copy(first);

  REQUIRE(&wrapper::access(first) == &wrapper::access(second));
  REQUIRE(wrapper::access(first).value == 9);

  wrapper::destroy(alloc, first);
  REQUIRE(tracked_value::destructions.load() == 0);
  wrapper::destroy(alloc, second);
  REQUIRE(tracked_value::destructions.load() == 1);
}

TEST_CASE("local and deep_copy ownership policies create independent targets",
          "[UT][wh/core/function/detail/ownership_policy.hpp][deep_copy][condition][branch]") {
  tracked_value::reset();

  {
    auto alloc = local_probe<tracked_value>::allocator_type{};
    local_probe<tracked_value>::stored_type first{};
    local_probe<tracked_value>::stored_type second{};

    local_probe<tracked_value>::create(alloc, &first, 5);
    local_probe<tracked_value>::copy(alloc, first, &second);
    REQUIRE(local_probe<tracked_value>::get(first).value == 5);
    REQUIRE(local_probe<tracked_value>::get(second).value == 5);
    first.value = 8;
    REQUIRE(local_probe<tracked_value>::get(first).value == 8);
    REQUIRE(local_probe<tracked_value>::get(second).value == 5);
    local_probe<tracked_value>::destroy(alloc, &first);
    local_probe<tracked_value>::destroy(alloc, &second);
  }
  REQUIRE(tracked_value::copies.load() >= 1);

  tracked_value::reset();
  {
    auto alloc = deep_probe<tracked_value>::allocator_type{};
    deep_probe<tracked_value>::stored_type first{};
    deep_probe<tracked_value>::stored_type second{};

    deep_probe<tracked_value>::create(alloc, &first, 7);
    deep_probe<tracked_value>::copy(alloc, first, &second);
    REQUIRE(deep_probe<tracked_value>::get(first).value == 7);
    REQUIRE(deep_probe<tracked_value>::get(second).value == 7);
    (*first).value = 12;
    REQUIRE(deep_probe<tracked_value>::get(first).value == 12);
    REQUIRE(deep_probe<tracked_value>::get(second).value == 7);
    deep_probe<tracked_value>::destroy(alloc, &first);
    deep_probe<tracked_value>::destroy(alloc, &second);
  }
  REQUIRE(tracked_value::copies.load() >= 1);
}

TEST_CASE(
    "reference_counting ownership policy shares a single target across copies",
    "[UT][wh/core/function/detail/ownership_policy.hpp][reference_counting][branch][boundary]") {
  tracked_value::reset();
  {
    auto alloc = ref_probe<tracked_value>::allocator_type{};
    ref_probe<tracked_value>::stored_type first{};
    ref_probe<tracked_value>::stored_type second{};

    ref_probe<tracked_value>::create(alloc, &first, 11);
    ref_probe<tracked_value>::copy(alloc, first, &second);
    REQUIRE(&ref_probe<tracked_value>::get(first) == &ref_probe<tracked_value>::get(second));
    ref_probe<tracked_value>::destroy(alloc, &first);
    REQUIRE(tracked_value::destructions.load() == 0);
    ref_probe<tracked_value>::destroy(alloc, &second);
  }
  REQUIRE(tracked_value::destructions.load() == 1);
}

TEST_CASE(
    "ownership policy identity trait distinguishes matching and different templates",
    "[UT][wh/core/function/detail/ownership_policy.hpp][is_same_ownership_policy_v][branch]") {
  REQUIRE(
      wh::core::fn::is_same_ownership_policy_v<wh::core::fn::deep_copy, wh::core::fn::deep_copy>);
  REQUIRE_FALSE(wh::core::fn::is_same_ownership_policy_v<wh::core::fn::deep_copy,
                                                         wh::core::fn::reference_counting>);
}
