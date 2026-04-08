#include <catch2/catch_test_macros.hpp>

#include "wh/core/function/detail/acceptance_policy.hpp"
#include "wh/core/function/detail/error_policy.hpp"
#include "wh/core/function/detail/function_base.hpp"
#include "wh/core/function/detail/ownership_policy.hpp"

namespace {

using storage_t = wh::core::fn::owning_storage<
    int(int), wh::core::fn::reference_counting, wh::core::fn::standard_accept,
    wh::core::fn::skip_on_error, sizeof(void *), std::allocator>;

struct plus_one {
  [[nodiscard]] auto operator()(int value) const noexcept -> int {
    return value + 1;
  }
};

[[nodiscard]] auto plus_one_free(const int value) noexcept -> int {
  return value + 1;
}

using function_ptr_t = int (*)(int);

struct function_base_probe : wh::core::fn_detail::function_base<storage_t> {
  using base = wh::core::fn_detail::function_base<storage_t>;
  using base::operator=;

  function_base_probe() = default;

  template <typename fun_t, typename... args_t>
  explicit function_base_probe(std::in_place_type_t<fun_t>, args_t &&...args)
      : base(std::in_place_type<fun_t>, std::forward<args_t>(args)...) {}

  explicit function_base_probe(std::nullptr_t) noexcept : base(nullptr) {}

  [[nodiscard]] auto empty() const noexcept -> bool {
    return storage_t::is_empty(this->local_buffer_);
  }

  auto swap_with(function_base_probe &other) noexcept -> void { this->swap(other); }

  template <typename fun_t, typename... args_t>
  [[nodiscard]] static consteval auto nothrow_constructible() -> bool {
    return base::template check_nothrow<fun_t, args_t...>();
  }
};

} // namespace

TEST_CASE("function_base manages empty copy move nullptr assignment and swap",
          "[UT][wh/core/function/detail/function_base.hpp][function_base][condition][branch]") {
  function_base_probe first{std::in_place_type<plus_one>, plus_one{}};
  REQUIRE_FALSE(first.empty());

  function_base_probe copied{first};
  REQUIRE_FALSE(copied.empty());

  function_base_probe moved{std::move(first)};
  REQUIRE_FALSE(moved.empty());

  function_base_probe empty{nullptr};
  REQUIRE(empty.empty());

  moved.swap_with(empty);
  REQUIRE(moved.empty());
  REQUIRE_FALSE(empty.empty());

  empty = nullptr;
  REQUIRE(empty.empty());

  static_assert(function_base_probe::nothrow_constructible<plus_one, plus_one>());
}

TEST_CASE("function_base copy and move assignment preserve callable emptiness state",
          "[UT][wh/core/function/detail/function_base.hpp][function_base::operator=][branch][boundary]") {
  function_base_probe source{std::in_place_type<plus_one>, plus_one{}};
  function_base_probe target{nullptr};
  REQUIRE(target.empty());

  target = source;
  REQUIRE_FALSE(target.empty());

  function_base_probe moved_target{nullptr};
  moved_target = std::move(target);
  REQUIRE_FALSE(moved_target.empty());
  REQUIRE(target.empty());

  moved_target = nullptr;
  REQUIRE(moved_target.empty());

  function_base_probe second_source{std::in_place_type<plus_one>, plus_one{}};
  source = second_source;
  REQUIRE_FALSE(source.empty());
}

TEST_CASE("function_base leaves null function pointers empty during in-place construction",
          "[UT][wh/core/function/detail/function_base.hpp][function_base][boundary]") {
  function_base_probe valid_function{
      std::in_place_type<function_ptr_t>, &plus_one_free};
  REQUIRE_FALSE(valid_function.empty());

  function_base_probe null_function{std::in_place_type<function_ptr_t>,
                                    static_cast<function_ptr_t>(nullptr)};
  REQUIRE(null_function.empty());
}
