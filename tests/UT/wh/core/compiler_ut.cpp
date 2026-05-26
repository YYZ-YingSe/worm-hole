#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/compiler.hpp"

static_assert(wh::core::trivially_relocatable<int>);
static_assert(wh::core::trivially_copyable_value<int>);
static_assert(wh::core::trivially_destructible_value<int>);
static_assert(wh::core::can_memcpy_value_v<int>);
static_assert(wh::core::can_memmove_value_v<int>);
static_assert(wh::core::can_trivially_relocate_value_v<int>);
static_assert(wh::core::can_skip_destroy_v<int>);
static_assert(!std::is_same_v<decltype(wh::core::active_compiler), int>);
static_assert(wh::core::pointer_alignment == alignof(void *));
static_assert(wh::core::default_inline_storage_alignment == alignof(std::max_align_t));
static_assert(wh::core::align_up_to<8U>(9U) == 16U);
static_assert(wh::core::align_up_to_pointer(1U) == wh::core::pointer_alignment);
static_assert(wh::core::fits_storage_alignment(wh::core::default_inline_storage_alignment,
                                               alignof(void *)));
static_assert(wh::core::fits_pointer_alignment(alignof(void *)));
static_assert(wh::core::fits_inline_storage<void *>(
    sizeof(void *), wh::core::default_inline_storage_alignment));
static_assert(wh::core::fits_pointer_aligned_storage<void *>(sizeof(void *)));
static_assert(wh::core::supports_native_stacktrace_capture ==
              (WH_SUPPORTS_NATIVE_STACKTRACE_CAPTURE != 0));
static_assert(wh::core::supports_intrinsic_spin_pause ==
              (WH_SUPPORTS_INTRINSIC_SPIN_PAUSE != 0));
static_assert(wh::core::supports_asm_spin_pause == (WH_SUPPORTS_ASM_SPIN_PAUSE != 0));
static_assert(wh::core::supports_native_spin_pause == (WH_SUPPORTS_NATIVE_SPIN_PAUSE != 0));

#if WH_COMPILER_MSVC_HEADERS
static_assert(wh::core::compiler_uses_msvc_headers);
#endif

#if WH_COMPILER_MSVC_HEADERS && WH_CPU_ARM64
static_assert(wh::core::active_cpu_architecture == wh::core::cpu_architecture::arm64);
static_assert(!wh::core::cpu_is_x86);
static_assert(wh::core::supports_intrinsic_spin_pause);
#endif

#if WH_COMPILER_MSVC_HEADERS && WH_CPU_ARM64EC
static_assert(wh::core::active_cpu_architecture == wh::core::cpu_architecture::arm64ec);
static_assert(!wh::core::cpu_is_x86);
static_assert(wh::core::cpu_is_arm);
static_assert(wh::core::supports_intrinsic_spin_pause);
#endif

TEST_CASE("compiler helpers expose platform alignment and contract metadata",
          "[UT][wh/core/compiler.hpp][is_power_of_two][branch][boundary]") {
  REQUIRE(wh::core::compiler_version_major >= 0);
  REQUIRE(wh::core::default_cacheline_size > 0U);
  REQUIRE(wh::core::pointer_alignment == alignof(void *));
  REQUIRE(wh::core::default_inline_storage_alignment == alignof(std::max_align_t));

  REQUIRE(wh::core::is_power_of_two(1U));
  REQUIRE(wh::core::is_power_of_two(2U));
  REQUIRE_FALSE(wh::core::is_power_of_two(0U));
  REQUIRE_FALSE(wh::core::is_power_of_two(3U));

  REQUIRE(wh::core::align_up(10U, 8U) == 16U);
  REQUIRE(wh::core::align_up(16U, 8U) == 16U);
  REQUIRE(wh::core::align_up(9U, 3U) == 9U);
  REQUIRE(wh::core::align_up_to<16U>(17U) == 32U);
  REQUIRE(wh::core::align_up_to_pointer(wh::core::pointer_alignment + 1U) ==
          (wh::core::pointer_alignment * 2U));
  REQUIRE_FALSE(wh::core::fits_pointer_alignment(0U));
  REQUIRE_FALSE(wh::core::fits_storage_alignment(0U, alignof(void *)));
  REQUIRE_FALSE(wh::core::fits_storage_alignment(wh::core::pointer_alignment, 0U));
  REQUIRE(wh::core::fits_pointer_alignment(1U));
  REQUIRE(wh::core::fits_pointer_alignment(alignof(void *)));

  REQUIRE(wh::core::next_power_of_two(0U) == 1U);
  REQUIRE(wh::core::next_power_of_two(1U) == 1U);
  REQUIRE(wh::core::next_power_of_two(1025U) == 2048U);
}

TEST_CASE("compiler helpers preserve boolean branch and contract semantics",
          "[UT][wh/core/compiler.hpp][contract_kind_name][condition][branch]") {
  REQUIRE(wh::core::contract_kind_name(wh::core::contract_kind::precondition) == "precondition");
  REQUIRE(wh::core::contract_kind_name(wh::core::contract_kind::postcondition) == "postcondition");
  REQUIRE(wh::core::contract_kind_name(wh::core::contract_kind::invariant) == "invariant");

  bool likely_taken = false;
  if (true)
    wh_likely { likely_taken = true; }
  REQUIRE(likely_taken);

  bool unlikely_taken = false;
  if (true)
    wh_unlikely { unlikely_taken = true; }
  REQUIRE(unlikely_taken);

  wh::core::assume(true);
  wh::core::spin_pause();
  wh_precondition(true);
  wh_postcondition(true);
  wh_invariant(true);
}

TEST_CASE("compiler helpers expose compiler identity consistently",
          "[UT][wh/core/compiler.hpp][active_compiler][condition][boundary]") {
  const auto active_flags =
      static_cast<int>(wh::core::active_compiler == wh::core::compiler_id::clang) +
      static_cast<int>(wh::core::active_compiler == wh::core::compiler_id::gcc) +
      static_cast<int>(wh::core::active_compiler == wh::core::compiler_id::msvc);

  if (wh::core::active_compiler == wh::core::compiler_id::unknown) {
    REQUIRE(active_flags == 0);
  } else {
    REQUIRE(active_flags == 1);
  }

  REQUIRE(wh::core::align_up(0U, 8U) == 0U);
  REQUIRE(wh::core::align_up(17U, 16U) == 32U);
  REQUIRE(wh::core::next_power_of_two(2U) == 2U);
  REQUIRE(wh::core::next_power_of_two(3U) == 4U);
}

TEST_CASE("compiler helpers expose target platform contracts consistently",
          "[UT][wh/core/compiler.hpp][cpu][os][spin_pause][boundary]") {
  const auto cpu_family_flags = static_cast<int>(wh::core::cpu_is_x86) +
                                static_cast<int>(wh::core::cpu_is_arm) +
                                static_cast<int>(wh::core::cpu_is_powerpc64);

  if (wh::core::active_cpu_architecture == wh::core::cpu_architecture::unknown) {
    REQUIRE(cpu_family_flags == 0);
    REQUIRE_FALSE(wh::core::cpu_is_known);
  } else {
    REQUIRE(cpu_family_flags == 1);
    REQUIRE(wh::core::cpu_is_known);
  }

  const auto os_flags = static_cast<int>(wh::core::operating_system_is_windows) +
                        static_cast<int>(wh::core::operating_system_is_darwin) +
                        static_cast<int>(wh::core::operating_system_is_unix);

  if (wh::core::active_operating_system == wh::core::operating_system_id::unknown) {
    REQUIRE(os_flags == 0);
  } else {
    REQUIRE(os_flags == 1);
  }

  REQUIRE(wh::core::operating_system_is_posix_like == (WH_OS_POSIX_LIKE != 0));
  REQUIRE(wh::core::supports_native_stacktrace_capture ==
          (WH_SUPPORTS_NATIVE_STACKTRACE_CAPTURE != 0));
  REQUIRE(wh::core::supports_native_spin_pause == (WH_SUPPORTS_NATIVE_SPIN_PAUSE != 0));
}
