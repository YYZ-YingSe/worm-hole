// Defines compiler-level portability helpers such as branch prediction,
// alignment utilities, and constexpr numeric helpers.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#include <immintrin.h>
#endif

namespace wh::core {

/// Identifies the active compiler for compile-time feature branching.
enum class compiler_id : std::uint8_t {
  /// Compiler could not be identified from predefined macros.
  unknown = 0,
  /// LLVM/Clang toolchain.
  clang = 1,
  /// GNU Compiler Collection.
  gcc = 2,
  /// Microsoft Visual C++ toolchain.
  msvc = 3,
};

#if defined(__clang__)
inline constexpr compiler_id active_compiler = compiler_id::clang;
inline constexpr int compiler_version_major = __clang_major__;
inline constexpr int compiler_version_minor = __clang_minor__;
inline constexpr int compiler_version_patch = __clang_patchlevel__;
#elif defined(__GNUC__)
inline constexpr compiler_id active_compiler = compiler_id::gcc;
inline constexpr int compiler_version_major = __GNUC__;
inline constexpr int compiler_version_minor = __GNUC_MINOR__;
inline constexpr int compiler_version_patch = __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
inline constexpr compiler_id active_compiler = compiler_id::msvc;
inline constexpr int compiler_version_major = _MSC_VER / 100;
inline constexpr int compiler_version_minor = _MSC_VER % 100;
inline constexpr int compiler_version_patch = 0;
#else
inline constexpr compiler_id active_compiler = compiler_id::unknown;
inline constexpr int compiler_version_major = 0;
inline constexpr int compiler_version_minor = 0;
inline constexpr int compiler_version_patch = 0;
#endif

inline constexpr bool compiler_is_clang = active_compiler == compiler_id::clang;
inline constexpr bool compiler_is_gcc = active_compiler == compiler_id::gcc;
inline constexpr bool compiler_is_msvc = active_compiler == compiler_id::msvc;

#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
inline constexpr bool supports_native_stacktrace_capture = true;
#else
inline constexpr bool supports_native_stacktrace_capture = false;
#endif

inline constexpr std::size_t default_cacheline_size = 64U;

/// True when a value can be moved by raw relocation safely.
template <typename t>
concept trivially_relocatable =
    std::is_trivially_move_constructible_v<t> && std::is_trivially_destructible_v<t>;

/// True when a value is bitwise copyable.
template <typename t>
concept trivially_copyable_value = std::is_trivially_copyable_v<t>;

/// Returns whether `value` is a non-zero power of two.
[[nodiscard]] constexpr bool is_power_of_two(const std::size_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

/// Aligns `value` upward to `alignment` when alignment is power-of-two.
[[nodiscard]] constexpr std::size_t align_up(const std::size_t value,
                                             const std::size_t alignment) noexcept {
  if (!is_power_of_two(alignment)) {
    return value;
  }
  return (value + alignment - 1U) & ~(alignment - 1U);
}

/// Computes the next power of two greater than or equal to `value`.
[[nodiscard]] constexpr std::size_t next_power_of_two(std::size_t value) noexcept {
  if (value <= 1U) {
    return 1U;
  }
  value -= 1U;
  for (std::size_t shift = 1U; shift < (sizeof(std::size_t) * 8U); shift <<= 1U) {
    value |= value >> shift;
  }
  return value + 1U;
}

/// Marks unreachable control flow and aborts as a fallback.
[[noreturn]] inline void unreachable() noexcept {
#if defined(__has_builtin)
#if __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#endif
#elif defined(__GNUC__) || defined(__clang__)
  __builtin_unreachable();
#elif defined(_MSC_VER)
  __assume(false);
#endif
  std::abort();
}

/// Hints the optimizer that `condition` is expected to hold.
template <typename condition_t>
  requires std::convertible_to<condition_t, bool>
inline void assume(condition_t &&condition) noexcept {
  const bool raw = static_cast<bool>(std::forward<condition_t>(condition));
#if defined(_MSC_VER)
  __assume(raw);
#elif defined(__has_builtin)
#if __has_builtin(__builtin_assume)
  __builtin_assume(raw);
#else
  if (!raw) {
    unreachable();
  }
#endif
#else
  if (!raw) {
    unreachable();
  }
#endif
}

/// Emits a short CPU pause/yield for spin-wait loops.
inline void spin_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(_MSC_VER)
  _mm_pause();
#else
  __asm__ __volatile__("pause" ::: "memory");
#endif
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#endif
}

enum class contract_kind : std::uint8_t {
  precondition = 0U,
  postcondition,
  invariant,
};

[[nodiscard]] inline auto contract_kind_name(const contract_kind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case contract_kind::precondition:
    return "precondition";
  case contract_kind::postcondition:
    return "postcondition";
  case contract_kind::invariant:
    return "invariant";
  }
  return "contract";
}

/// Reports contract violation and terminates immediately.
[[noreturn]] inline void
contract_violation(const contract_kind kind, const std::string_view expression,
                   const std::source_location where = std::source_location::current()) noexcept {
  const auto kind_name = contract_kind_name(kind);
  std::fprintf(stderr, "[wh-contract] %.*s failed: %.*s at %s:%u\n",
               static_cast<int>(kind_name.size()), kind_name.data(),
               static_cast<int>(expression.size()), expression.data(), where.file_name(),
               where.line());
  std::abort();
}

} // namespace wh::core

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(likely)
#define wh_likely [[likely]]
#else
#define wh_likely
#endif

#if __has_cpp_attribute(unlikely)
#define wh_unlikely [[unlikely]]
#else
#define wh_unlikely
#endif
#else
#define wh_likely
#define wh_unlikely
#endif

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(no_unique_address)
#define wh_no_unique_address [[no_unique_address]]
#elif defined(_MSC_VER) && __has_cpp_attribute(msvc::no_unique_address)
#define wh_no_unique_address [[msvc::no_unique_address]]
#else
#define wh_no_unique_address
#endif
#else
#define wh_no_unique_address
#endif

#if defined(_MSC_VER)
#define wh_empty_bases __declspec(empty_bases)
#else
#define wh_empty_bases
#endif

#if defined(__GNUC__) || defined(__clang__)
#define wh_force_inline [[gnu::always_inline]] inline
#define wh_noinline [[gnu::noinline]]
#define wh_hot [[gnu::hot]]
#define wh_cold [[gnu::cold]]
#define wh_pure [[gnu::pure]]
#define wh_const [[gnu::const]]
#define wh_restrict __restrict__
#define wh_cacheline_align alignas(::wh::core::default_cacheline_size)
#elif defined(_MSC_VER)
#define wh_force_inline __forceinline
#define wh_noinline __declspec(noinline)
#define wh_hot
#define wh_cold
#define wh_pure
#define wh_const
#define wh_restrict __restrict
#define wh_cacheline_align alignas(::wh::core::default_cacheline_size)
#else
#define wh_force_inline inline
#define wh_noinline
#define wh_hot
#define wh_cold
#define wh_pure
#define wh_const
#define wh_restrict
#define wh_cacheline_align
#endif

#ifndef NDEBUG
#define wh_precondition(expr)                                                                      \
  (static_cast<bool>(expr) ||                                                                      \
   (::wh::core::contract_violation(::wh::core::contract_kind::precondition, #expr,                 \
                                   std::source_location::current()),                               \
    false))

#define wh_postcondition(expr)                                                                     \
  (static_cast<bool>(expr) ||                                                                      \
   (::wh::core::contract_violation(::wh::core::contract_kind::postcondition, #expr,                \
                                   std::source_location::current()),                               \
    false))

#define wh_invariant(expr)                                                                         \
  (static_cast<bool>(expr) ||                                                                      \
   (::wh::core::contract_violation(::wh::core::contract_kind::invariant, #expr,                    \
                                   std::source_location::current()),                               \
    false))
#else
#define wh_precondition(expr) ((void)0)
#define wh_postcondition(expr) ((void)0)
#define wh_invariant(expr) ((void)0)
#endif
