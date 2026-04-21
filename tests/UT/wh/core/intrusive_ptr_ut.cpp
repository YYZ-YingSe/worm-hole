#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/intrusive_ptr.hpp"

namespace {

struct construction_probe : wh::core::detail::intrusive_enable_from_this<construction_probe> {
  static inline const construction_probe *observed_self{nullptr};

  construction_probe() { observed_self = this->intrusive_from_this().get(); }
};

struct destructor_probe : wh::core::detail::intrusive_enable_from_this<destructor_probe> {
  static inline std::size_t destroyed{0U};

  ~destructor_probe() { ++destroyed; }
};

struct reserved_bit_probe : wh::core::detail::intrusive_enable_from_this<reserved_bit_probe, 1U> {
  [[nodiscard]] auto marked() const noexcept -> bool { return this->template is_set<0U>(); }

  auto mark() noexcept -> void { static_cast<void>(this->template set_bit<0U>()); }

  auto clear() noexcept -> void { static_cast<void>(this->template clear_bit<0U>()); }
};

} // namespace

TEST_CASE("make_intrusive supports intrusive_from_this during value construction",
          "[UT][wh/core/intrusive_ptr.hpp][make_intrusive][intrusive_from_this][lifetime]") {
  construction_probe::observed_self = nullptr;

  auto value = wh::core::detail::make_intrusive<construction_probe>();

  REQUIRE(construction_probe::observed_self == value.get());
}

TEST_CASE("intrusive_ptr keeps control block alive across const view copies",
          "[UT][wh/core/intrusive_ptr.hpp][intrusive_ptr][const][lifetime]") {
  destructor_probe::destroyed = 0U;

  auto value = wh::core::detail::make_intrusive<destructor_probe>();
  wh::core::detail::intrusive_ptr<const destructor_probe> const_view = value;

  REQUIRE(value.get() == const_view.get());

  value.reset();
  REQUIRE(destructor_probe::destroyed == 0U);

  const_view.reset();
  REQUIRE(destructor_probe::destroyed == 1U);
}

TEST_CASE("intrusive reserved bits are stored on the control block",
          "[UT][wh/core/intrusive_ptr.hpp][reserved_bits][branch]") {
  auto value = wh::core::detail::make_intrusive<reserved_bit_probe, 1U>();

  REQUIRE_FALSE(value->marked());

  value->mark();
  REQUIRE(value->marked());

  auto copy = value;
  value.reset();

  REQUIRE(copy->marked());

  copy->clear();
  REQUIRE_FALSE(copy->marked());
}
