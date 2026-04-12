#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "wh/core/function/detail/acceptance_policy.hpp"

namespace {

struct copyable_callable {
  [[nodiscard]] auto operator()(int value) const -> int { return value + 1; }
};

struct move_only_callable {
  move_only_callable() = default;
  move_only_callable(const move_only_callable &) = delete;
  auto operator=(const move_only_callable &) -> move_only_callable & = delete;
  move_only_callable(move_only_callable &&) noexcept = default;
  auto operator=(move_only_callable &&) noexcept -> move_only_callable & = default;

  [[nodiscard]] auto operator()(int value) const -> int { return value + 2; }
};

struct noexcept_callable {
  [[nodiscard]] auto operator()(int value) const noexcept -> int {
    return value + 3;
  }
};

struct lvalue_only_callable {
  [[nodiscard]] auto operator()(int value) & -> int { return value + 4; }
};

struct rvalue_only_callable {
  [[nodiscard]] auto operator()(int value) && -> int { return value + 5; }
};

struct const_lvalue_callable {
  [[nodiscard]] auto operator()(int value) const & -> int { return value + 6; }
};

template <typename signature_t>
struct standard_probe : wh::core::fn::standard_accept<signature_t> {
  template <typename fun_t>
  [[nodiscard]] static consteval auto eligible() -> bool {
    return standard_probe::template is_eligible<fun_t>;
  }
};

template <typename signature_t>
struct move_probe : wh::core::fn::non_copyable_accept<signature_t> {
  template <typename fun_t>
  [[nodiscard]] static consteval auto eligible() -> bool {
    return move_probe::template is_eligible<fun_t>;
  }
};

template <typename signature_t>
struct ptr_probe : wh::core::fn::ptr_accept<signature_t> {
  template <typename fun_t>
  [[nodiscard]] static consteval auto eligible() -> bool {
    return ptr_probe::template is_eligible<fun_t>;
  }
};

template <typename signature_t>
struct move_ptr_probe : wh::core::fn::non_copyable_ptr_accept<signature_t> {
  template <typename fun_t>
  [[nodiscard]] static consteval auto eligible() -> bool {
    return move_ptr_probe::template is_eligible<fun_t>;
  }
};

static_assert(standard_probe<int(int)>::template eligible<copyable_callable>());
static_assert(!standard_probe<int(int)>::template eligible<move_only_callable>());
static_assert(move_probe<int(int)>::template eligible<move_only_callable>());
static_assert(!standard_probe<int(int)>::template eligible<
              std::unique_ptr<copyable_callable>>());
static_assert(ptr_probe<int(int)>::template eligible<
              std::unique_ptr<copyable_callable>>());
static_assert(move_ptr_probe<int(int)>::template eligible<
              std::unique_ptr<move_only_callable>>());
static_assert(!standard_probe<int(int) noexcept>::template eligible<copyable_callable>());
static_assert(standard_probe<int(int) noexcept>::template eligible<
              noexcept_callable>());
static_assert(standard_probe<int(int) &>::template eligible<lvalue_only_callable>());
static_assert(!standard_probe<int(int) &&>::template eligible<lvalue_only_callable>());
static_assert(standard_probe<int(int) &&>::template eligible<rvalue_only_callable>());
static_assert(standard_probe<int(int) const &>::template eligible<
              const_lvalue_callable>());

} // namespace

TEST_CASE("standard_accept respects copyability and cv-ref qualified signatures",
          "[UT][wh/core/function/detail/acceptance_policy.hpp][standard_accept][condition][branch]") {
  REQUIRE(standard_probe<int(int)>::template eligible<copyable_callable>());
  REQUIRE_FALSE(
      standard_probe<int(int)>::template eligible<move_only_callable>());
  REQUIRE(
      standard_probe<int(int) &>::template eligible<lvalue_only_callable>());
  REQUIRE_FALSE(
      standard_probe<int(int) &&>::template eligible<lvalue_only_callable>());
  REQUIRE(
      standard_probe<int(int) &&>::template eligible<rvalue_only_callable>());
  REQUIRE(standard_probe<int(int) const &>::template eligible<
          const_lvalue_callable>());
}

TEST_CASE("non_copyable acceptance policies admit move-only callables when requested",
          "[UT][wh/core/function/detail/acceptance_policy.hpp][non_copyable_accept][branch][boundary]") {
  REQUIRE(move_probe<int(int)>::template eligible<move_only_callable>());
  REQUIRE(move_ptr_probe<int(int)>::template eligible<
          std::unique_ptr<move_only_callable>>());
  REQUIRE_FALSE(move_probe<int(int)>::template eligible<int>());
}

TEST_CASE("pointer and noexcept acceptance policies distinguish pointee invocation requirements",
          "[UT][wh/core/function/detail/acceptance_policy.hpp][ptr_accept][condition][branch][boundary]") {
  REQUIRE_FALSE(standard_probe<int(int)>::template eligible<
                std::unique_ptr<copyable_callable>>());
  REQUIRE(ptr_probe<int(int)>::template eligible<
          std::unique_ptr<copyable_callable>>());
  REQUIRE_FALSE(
      standard_probe<int(int) noexcept>::template eligible<copyable_callable>());
  REQUIRE(standard_probe<int(int) noexcept>::template eligible<
          noexcept_callable>());
}
